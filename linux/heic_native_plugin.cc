#include "include/heic_native/heic_native_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include <libheif/heif.h>
#include <png.h>

#define HEIC_NATIVE_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), heic_native_plugin_get_type(), \
                              HeicNativePlugin))

struct _HeicNativePlugin {
  GObject parent_instance;
  GAsyncQueue* work_queue;
  GThread* worker_thread;
  gboolean disposed;
};

G_DEFINE_TYPE(HeicNativePlugin, heic_native_plugin, g_object_get_type())

// Metadata extracted from a HEIC image.
struct HeicMetadata {
  std::vector<uint8_t> icc_profile;
  std::vector<uint8_t> exif_data;
};

// Custom write callback state for in-memory PNG encoding.
struct PngWriteState {
  std::vector<uint8_t>* buffer;
  bool error = false;
};

// Work item for the background worker thread.
struct ConvertWorkItem {
  char* input_path;       // g_strdup'd, owned
  char* output_path;      // g_strdup'd or nullptr for convertToBytes
  int compression_level;
  bool preserve_metadata;
  bool is_bytes;
  FlMethodCall* method_call;  // g_object_ref'd
  bool shutdown;              // sentinel flag
};

// Result delivery back to the main thread via g_idle_add.
struct ResultDelivery {
  FlMethodCall* method_call;
  FlMethodResponse* response;
  HeicNativePlugin* plugin;
};

static gboolean deliver_result_idle(gpointer data) {
  auto* d = reinterpret_cast<ResultDelivery*>(data);
  if (!d->plugin->disposed) {
    fl_method_call_respond(d->method_call, d->response, nullptr);
  }
  g_object_unref(d->method_call);
  g_object_unref(d->response);
  g_object_unref(d->plugin);
  delete d;
  return G_SOURCE_REMOVE;
}

static void png_write_to_vector(png_structp png_ptr, png_bytep data,
                                png_size_t length) {
  PngWriteState* state =
      reinterpret_cast<PngWriteState*>(png_get_io_ptr(png_ptr));
  try {
    state->buffer->insert(state->buffer->end(), data, data + length);
  } catch (...) {
    state->error = true;
  }
}

static void png_flush_noop(png_structp /*png_ptr*/) {}

// Describes a libheif failure for the error sent back to Dart: which step
// failed, libheif's own message, and the libheif version actually loaded.
// The version matters on Linux, where a packaging mistake can land the plugin
// on a system libheif instead of the bundled one.
static std::string describe_heif_error(const char* step, const heif_error& err) {
  std::string text = step;
  text += ": ";
  text += (err.message != nullptr) ? err.message : "unknown error";
  text += " (libheif ";
  text += heif_get_version();
  text += ")";
  return text;
}

// Decode a HEIC file into raw RGBA pixels.
// Returns true on success; fills out width, height, stride, and pixels.
// On failure, out_error says which step failed and why.
// Caller must call heif_image_release(out_img) and
// heif_image_handle_release(out_handle) when done.
static bool decode_heic(const char* input_path,
                        heif_context** out_ctx,
                        heif_image_handle** out_handle,
                        heif_image** out_img,
                        int* out_width,
                        int* out_height,
                        int* out_stride,
                        const uint8_t** out_pixels,
                        bool* out_has_alpha,
                        std::string* out_error) {
  heif_context* ctx = heif_context_alloc();
  if (!ctx) {
    *out_error = "heif_context_alloc returned null";
    return false;
  }

  heif_error err = heif_context_read_from_file(ctx, input_path, nullptr);
  if (err.code != heif_error_Ok) {
    *out_error = describe_heif_error("reading the file", err);
    heif_context_free(ctx);
    return false;
  }

  heif_image_handle* handle = nullptr;
  err = heif_context_get_primary_image_handle(ctx, &handle);
  if (err.code != heif_error_Ok) {
    *out_error = describe_heif_error("getting the primary image", err);
    heif_context_free(ctx);
    return false;
  }

  bool has_alpha = heif_image_handle_has_alpha_channel(handle);

  heif_image* img = nullptr;
  err = heif_decode_image(handle, &img, heif_colorspace_RGB,
                          has_alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB,
                          nullptr);
  if (err.code != heif_error_Ok) {
    *out_error = describe_heif_error("decoding the image", err);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return false;
  }

  int stride = 0;
  const uint8_t* pixels =
      heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
  if (!pixels) {
    heif_image_release(img);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return false;
  }

  *out_ctx = ctx;
  *out_handle = handle;
  *out_img = img;
  *out_width = heif_image_handle_get_width(handle);
  *out_height = heif_image_handle_get_height(handle);
  *out_stride = stride;
  *out_pixels = pixels;
  *out_has_alpha = has_alpha;
  return true;
}

// Normalize the EXIF orientation tag (274) to 1 (Normal) in a raw EXIF blob.
// The blob has a 4-byte big-endian TIFF-offset header (as returned by libheif).
// Returns true if the tag was found and patched; false if not found (not an error).
static bool normalize_exif_orientation(std::vector<uint8_t>& exif_data) {
    if (exif_data.size() < 8) return false;

    // Skip the 4-byte header offset that libheif prepends.
    uint32_t hdr_offset =
        (static_cast<uint32_t>(exif_data[0]) << 24) |
        (static_cast<uint32_t>(exif_data[1]) << 16) |
        (static_cast<uint32_t>(exif_data[2]) << 8)  |
        static_cast<uint32_t>(exif_data[3]);
    size_t tiff_start = 4 + static_cast<size_t>(hdr_offset);
    if (tiff_start + 8 > exif_data.size()) return false;

    // Determine byte order.
    bool little_endian = (exif_data[tiff_start] == 0x49);

    auto read16 = [&](size_t off) -> uint16_t {
        if (off + 2 > exif_data.size()) return 0;
        if (little_endian)
            return static_cast<uint16_t>(exif_data[off]) |
                   (static_cast<uint16_t>(exif_data[off + 1]) << 8);
        return (static_cast<uint16_t>(exif_data[off]) << 8) |
                static_cast<uint16_t>(exif_data[off + 1]);
    };

    auto read32 = [&](size_t off) -> uint32_t {
        if (off + 4 > exif_data.size()) return 0;
        if (little_endian)
            return static_cast<uint32_t>(exif_data[off]) |
                   (static_cast<uint32_t>(exif_data[off + 1]) << 8)  |
                   (static_cast<uint32_t>(exif_data[off + 2]) << 16) |
                   (static_cast<uint32_t>(exif_data[off + 3]) << 24);
        return (static_cast<uint32_t>(exif_data[off]) << 24) |
               (static_cast<uint32_t>(exif_data[off + 1]) << 16) |
               (static_cast<uint32_t>(exif_data[off + 2]) << 8)  |
                static_cast<uint32_t>(exif_data[off + 3]);
    };

    auto write16 = [&](size_t off, uint16_t val) {
        if (off + 2 > exif_data.size()) return;
        if (little_endian) {
            exif_data[off]     = static_cast<uint8_t>(val & 0xFF);
            exif_data[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
        } else {
            exif_data[off]     = static_cast<uint8_t>((val >> 8) & 0xFF);
            exif_data[off + 1] = static_cast<uint8_t>(val & 0xFF);
        }
    };

    // IFD0 offset is at bytes 4-7 of the TIFF block (relative to tiff_start).
    uint32_t ifd0_offset = read32(tiff_start + 4);
    size_t ifd0_pos = tiff_start + ifd0_offset;
    if (ifd0_pos + 2 > exif_data.size()) return false;

    uint16_t entry_count = read16(ifd0_pos);
    for (uint16_t i = 0; i < entry_count; i++) {
        size_t entry_pos = ifd0_pos + 2 + static_cast<size_t>(i) * 12;
        if (entry_pos + 12 > exif_data.size()) break;
        uint16_t tag = read16(entry_pos);
        if (tag == 0x0112) {  // Orientation
            // Type must be SHORT (3), count must be 1.
            // Value is at entry_pos + 8 (4-byte value field, SHORT in first 2 bytes).
            write16(entry_pos + 8, 1);  // 1 = Normal
            return true;
        }
    }
    return false;
}

// Patch EXIF dimension tags to match the decoded (possibly rotated) image size.
// Updates IFD0 tags 0x0100/0x0101 and ExifIFD tags 0xA002/0xA003.
static void update_exif_dimensions(std::vector<uint8_t>& exif_data, int width, int height) {
    if (exif_data.size() < 8) return;

    uint32_t hdr_offset =
        (static_cast<uint32_t>(exif_data[0]) << 24) |
        (static_cast<uint32_t>(exif_data[1]) << 16) |
        (static_cast<uint32_t>(exif_data[2]) << 8)  |
        static_cast<uint32_t>(exif_data[3]);
    size_t tiff_start = 4 + static_cast<size_t>(hdr_offset);
    if (tiff_start + 8 > exif_data.size()) return;

    bool little_endian = (exif_data[tiff_start] == 0x49);

    auto read16 = [&](size_t off) -> uint16_t {
        if (off + 2 > exif_data.size()) return 0;
        if (little_endian)
            return static_cast<uint16_t>(exif_data[off]) |
                   (static_cast<uint16_t>(exif_data[off + 1]) << 8);
        return (static_cast<uint16_t>(exif_data[off]) << 8) |
                static_cast<uint16_t>(exif_data[off + 1]);
    };

    auto read32 = [&](size_t off) -> uint32_t {
        if (off + 4 > exif_data.size()) return 0;
        if (little_endian)
            return static_cast<uint32_t>(exif_data[off]) |
                   (static_cast<uint32_t>(exif_data[off + 1]) << 8)  |
                   (static_cast<uint32_t>(exif_data[off + 2]) << 16) |
                   (static_cast<uint32_t>(exif_data[off + 3]) << 24);
        return (static_cast<uint32_t>(exif_data[off]) << 24) |
               (static_cast<uint32_t>(exif_data[off + 1]) << 16) |
               (static_cast<uint32_t>(exif_data[off + 2]) << 8)  |
                static_cast<uint32_t>(exif_data[off + 3]);
    };

    auto write16 = [&](size_t off, uint16_t val) {
        if (off + 2 > exif_data.size()) return;
        if (little_endian) {
            exif_data[off]     = static_cast<uint8_t>(val & 0xFF);
            exif_data[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
        } else {
            exif_data[off]     = static_cast<uint8_t>((val >> 8) & 0xFF);
            exif_data[off + 1] = static_cast<uint8_t>(val & 0xFF);
        }
    };

    auto write32 = [&](size_t off, uint32_t val) {
        if (off + 4 > exif_data.size()) return;
        if (little_endian) {
            exif_data[off]     = static_cast<uint8_t>(val & 0xFF);
            exif_data[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
            exif_data[off + 2] = static_cast<uint8_t>((val >> 16) & 0xFF);
            exif_data[off + 3] = static_cast<uint8_t>((val >> 24) & 0xFF);
        } else {
            exif_data[off]     = static_cast<uint8_t>((val >> 24) & 0xFF);
            exif_data[off + 1] = static_cast<uint8_t>((val >> 16) & 0xFF);
            exif_data[off + 2] = static_cast<uint8_t>((val >> 8) & 0xFF);
            exif_data[off + 3] = static_cast<uint8_t>(val & 0xFF);
        }
    };

    uint32_t ifd0_offset = read32(tiff_start + 4);
    size_t ifd0_pos = tiff_start + ifd0_offset;
    if (ifd0_pos + 2 > exif_data.size()) return;

    uint16_t entry_count = read16(ifd0_pos);
    size_t exif_ifd_pos = 0;

    for (uint16_t i = 0; i < entry_count; i++) {
        size_t entry_pos = ifd0_pos + 2 + static_cast<size_t>(i) * 12;
        if (entry_pos + 12 > exif_data.size()) break;
        uint16_t tag  = read16(entry_pos);
        uint16_t type = read16(entry_pos + 2);
        if (tag == 0x0100 || tag == 0x0101) {  // ImageWidth / ImageLength
            uint32_t val = static_cast<uint32_t>(tag == 0x0100 ? width : height);
            if (type == 3) {        // SHORT
                write16(entry_pos + 8, static_cast<uint16_t>(val));
            } else if (type == 4) { // LONG
                write32(entry_pos + 8, val);
            }
        } else if (tag == 0x8769) {  // ExifIFDPointer
            uint32_t exif_ifd_offset = read32(entry_pos + 8);
            exif_ifd_pos = tiff_start + exif_ifd_offset;
        }
    }

    if (exif_ifd_pos != 0 && exif_ifd_pos + 2 <= exif_data.size()) {
        uint16_t exif_entry_count = read16(exif_ifd_pos);
        for (uint16_t i = 0; i < exif_entry_count; i++) {
            size_t entry_pos = exif_ifd_pos + 2 + static_cast<size_t>(i) * 12;
            if (entry_pos + 12 > exif_data.size()) break;
            uint16_t tag  = read16(entry_pos);
            uint16_t type = read16(entry_pos + 2);
            if (tag == 0xA002 || tag == 0xA003) {  // PixelXDimension / PixelYDimension
                uint32_t val = static_cast<uint32_t>(tag == 0xA002 ? width : height);
                if (type == 3) {        // SHORT
                    write16(entry_pos + 8, static_cast<uint16_t>(val));
                } else if (type == 4) { // LONG
                    write32(entry_pos + 8, val);
                }
            }
        }
    }
}

// Extract ICC profile and EXIF metadata from a decoded HEIC image handle.
static HeicMetadata extract_metadata(heif_image_handle* handle, int width, int height) {
  HeicMetadata meta;

  // ICC profile
  heif_color_profile_type profile_type =
      heif_image_handle_get_color_profile_type(handle);
  if (profile_type == heif_color_profile_type_prof ||
      profile_type == heif_color_profile_type_rICC) {
    size_t profile_size = heif_image_handle_get_raw_color_profile_size(handle);
    if (profile_size > 0) {
      meta.icc_profile.resize(profile_size);
      heif_error err = heif_image_handle_get_raw_color_profile(
          handle, meta.icc_profile.data());
      if (err.code != heif_error_Ok) {
        meta.icc_profile.clear();
      }
    }
  }

  // EXIF data
  heif_item_id exif_id;
  int n = heif_image_handle_get_list_of_metadata_block_IDs(
      handle, "Exif", &exif_id, 1);
  if (n > 0) {
    size_t exif_size = heif_image_handle_get_metadata_size(handle, exif_id);
    if (exif_size > 0) {
      meta.exif_data.resize(exif_size);
      heif_error err =
          heif_image_handle_get_metadata(handle, exif_id, meta.exif_data.data());
      if (err.code != heif_error_Ok) {
        meta.exif_data.clear();
      }
    }
  }

  if (!meta.exif_data.empty()) {
    normalize_exif_orientation(meta.exif_data);
    update_exif_dimensions(meta.exif_data, width, height);
  }

  return meta;
}

// Write RGBA pixels as a PNG to an already-opened FILE*.
// Returns true on success.
static bool write_png_to_file(FILE* fp, int width, int height, int stride,
                              const uint8_t* pixels, int compression_level,
                              const HeicMetadata* metadata, bool has_alpha) {
  png_structp png_ptr =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png_ptr) return false;

  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    png_destroy_write_struct(&png_ptr, nullptr);
    return false;
  }

  // SAFETY: libpng uses setjmp/longjmp for error handling. All locals that
  // must be cleaned up on error are either trivially destructible (plain
  // pointers/ints) or are not declared between here and the longjmp target,
  // so C++ destructors are not skipped in a way that would cause leaks.
  if (setjmp(png_jmpbuf(png_ptr))) {
    png_destroy_write_struct(&png_ptr, &info_ptr);
    return false;
  }

  png_init_io(png_ptr, fp);
  png_set_IHDR(png_ptr, info_ptr, width, height, 8,
               has_alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);

  png_set_compression_level(png_ptr, compression_level);
  if (compression_level == 0) {
    png_set_filter(png_ptr, 0, PNG_FILTER_NONE);
  } else {
    png_set_filter(png_ptr, 0, PNG_ALL_FILTERS);
  }

  if (metadata) {
    if (!metadata->icc_profile.empty()) {
      png_set_iCCP(png_ptr, info_ptr, "icc",
                   PNG_COMPRESSION_TYPE_BASE,
                   const_cast<png_bytep>(metadata->icc_profile.data()),
                   static_cast<png_uint_32>(metadata->icc_profile.size()));
    }
#if PNG_LIBPNG_VER >= 10632
    if (metadata->exif_data.size() > 4) {
      // First 4 bytes are a big-endian TIFF header offset.
      uint32_t tiff_offset =
          (static_cast<uint32_t>(metadata->exif_data[0]) << 24) |
          (static_cast<uint32_t>(metadata->exif_data[1]) << 16) |
          (static_cast<uint32_t>(metadata->exif_data[2]) << 8) |
          static_cast<uint32_t>(metadata->exif_data[3]);
      if (tiff_offset <= metadata->exif_data.size() - 4) {
        size_t skip = 4 + static_cast<size_t>(tiff_offset);
        png_set_eXIf_1(png_ptr, info_ptr,
                       static_cast<png_uint_32>(metadata->exif_data.size() - skip),
                       const_cast<png_bytep>(metadata->exif_data.data() + skip));
      }
    }
#endif
  }

  png_write_info(png_ptr, info_ptr);

  for (int y = 0; y < height; y++) {
    png_write_row(png_ptr,
                  const_cast<png_bytep>(pixels + static_cast<size_t>(y) * stride));
  }

  png_write_end(png_ptr, nullptr);
  png_destroy_write_struct(&png_ptr, &info_ptr);
  return true;
}

// Write RGBA pixels as a PNG into a memory buffer.
// Returns true on success.
static bool write_png_to_memory(std::vector<uint8_t>* buf, int width,
                                int height, int stride,
                                const uint8_t* pixels, int compression_level,
                                const HeicMetadata* metadata, bool has_alpha) {
  png_structp png_ptr =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png_ptr) return false;

  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    png_destroy_write_struct(&png_ptr, nullptr);
    return false;
  }

  // SAFETY: libpng uses setjmp/longjmp for error handling. All locals that
  // must be cleaned up on error are either trivially destructible (plain
  // pointers/ints) or are not declared between here and the longjmp target,
  // so C++ destructors are not skipped in a way that would cause leaks.
  if (setjmp(png_jmpbuf(png_ptr))) {
    png_destroy_write_struct(&png_ptr, &info_ptr);
    return false;
  }

  PngWriteState state{buf};
  png_set_write_fn(png_ptr, &state, png_write_to_vector, png_flush_noop);

  png_set_IHDR(png_ptr, info_ptr, width, height, 8,
               has_alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);

  png_set_compression_level(png_ptr, compression_level);
  if (compression_level == 0) {
    png_set_filter(png_ptr, 0, PNG_FILTER_NONE);
  } else {
    png_set_filter(png_ptr, 0, PNG_ALL_FILTERS);
  }

  if (metadata) {
    if (!metadata->icc_profile.empty()) {
      png_set_iCCP(png_ptr, info_ptr, "icc",
                   PNG_COMPRESSION_TYPE_BASE,
                   const_cast<png_bytep>(metadata->icc_profile.data()),
                   static_cast<png_uint_32>(metadata->icc_profile.size()));
    }
#if PNG_LIBPNG_VER >= 10632
    if (metadata->exif_data.size() > 4) {
      // First 4 bytes are a big-endian TIFF header offset.
      uint32_t tiff_offset =
          (static_cast<uint32_t>(metadata->exif_data[0]) << 24) |
          (static_cast<uint32_t>(metadata->exif_data[1]) << 16) |
          (static_cast<uint32_t>(metadata->exif_data[2]) << 8) |
          static_cast<uint32_t>(metadata->exif_data[3]);
      if (tiff_offset <= metadata->exif_data.size() - 4) {
        size_t skip = 4 + static_cast<size_t>(tiff_offset);
        png_set_eXIf_1(png_ptr, info_ptr,
                       static_cast<png_uint_32>(metadata->exif_data.size() - skip),
                       const_cast<png_bytep>(metadata->exif_data.data() + skip));
      }
    }
#endif
  }

  png_write_info(png_ptr, info_ptr);
  if (state.error) png_error(png_ptr, "memory allocation failed");

  for (int y = 0; y < height; y++) {
    png_write_row(png_ptr,
                  const_cast<png_bytep>(pixels + static_cast<size_t>(y) * stride));
    if (state.error) png_error(png_ptr, "memory allocation failed");
  }

  png_write_end(png_ptr, nullptr);
  if (state.error) png_error(png_ptr, "memory allocation failed");
  png_destroy_write_struct(&png_ptr, &info_ptr);
  return true;
}

// Perform file conversion on worker thread; returns a FlMethodResponse*.
static FlMethodResponse* do_convert_work(const char* input_path,
                                          const char* output_path,
                                          int compression_level,
                                          bool preserve_metadata) {
  heif_context* ctx = nullptr;
  heif_image_handle* handle = nullptr;
  heif_image* img = nullptr;
  int width = 0, height = 0, stride = 0;
  const uint8_t* pixels = nullptr;
  bool has_alpha = false;

  std::string decode_error;
  bool ok = decode_heic(input_path, &ctx, &handle, &img, &width, &height,
                        &stride, &pixels, &has_alpha, &decode_error);
  if (!ok) {
    std::string message = "Failed to decode HEIC file, " + decode_error;
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "decode_failed", message.c_str(), nullptr));
  }

  HeicMetadata meta;
  if (preserve_metadata) {
    meta = extract_metadata(handle, width, height);
  }

  char temp_path[PATH_MAX];
  int snp_ret = snprintf(temp_path, sizeof(temp_path), "%s.heic_native.XXXXXX", output_path);
  if (snp_ret < 0 || static_cast<size_t>(snp_ret) >= sizeof(temp_path)) {
    heif_image_release(img);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "encode_failed", "Output path too long", nullptr));
  }
  int fd = mkstemp(temp_path);
  bool success = false;
  if (fd >= 0) {
    FILE* fp = fdopen(fd, "wb");
    if (fp) {
      success = write_png_to_file(fp, width, height, stride, pixels,
                                  compression_level,
                                  preserve_metadata ? &meta : nullptr,
                                  has_alpha);
      fclose(fp);
    } else {
      close(fd);
    }
    if (success) {
      if (rename(temp_path, output_path) != 0) {
        unlink(temp_path);
        success = false;
      }
    } else {
      unlink(temp_path);
    }
  }

  heif_image_release(img);
  heif_image_handle_release(handle);
  heif_context_free(ctx);

  if (!success) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "encode_failed", "Failed to write PNG file", nullptr));
  }

  g_autoptr(FlValue) result = fl_value_new_bool(true);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

// Perform bytes conversion on worker thread; returns a FlMethodResponse*.
static FlMethodResponse* do_convert_to_bytes_work(const char* input_path,
                                                    int compression_level,
                                                    bool preserve_metadata) {
  heif_context* ctx = nullptr;
  heif_image_handle* handle = nullptr;
  heif_image* img = nullptr;
  int width = 0, height = 0, stride = 0;
  const uint8_t* pixels = nullptr;
  bool has_alpha = false;

  std::string decode_error;
  bool ok = decode_heic(input_path, &ctx, &handle, &img, &width, &height,
                        &stride, &pixels, &has_alpha, &decode_error);
  if (!ok) {
    std::string message = "Failed to decode HEIC file, " + decode_error;
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "decode_failed", message.c_str(), nullptr));
  }

  HeicMetadata meta;
  if (preserve_metadata) {
    meta = extract_metadata(handle, width, height);
  }

  std::vector<uint8_t> buf;
  bool success = write_png_to_memory(&buf, width, height, stride, pixels,
                                     compression_level,
                                     preserve_metadata ? &meta : nullptr,
                                     has_alpha);

  heif_image_release(img);
  heif_image_handle_release(handle);
  heif_context_free(ctx);

  if (!success || buf.empty()) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "encode_failed", "Failed to encode PNG", nullptr));
  }

  g_autoptr(FlValue) result =
      fl_value_new_uint8_list(buf.data(), buf.size());
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static gpointer worker_thread_func(gpointer data) {
  HeicNativePlugin* self = HEIC_NATIVE_PLUGIN(data);

  while (true) {
    auto* item = reinterpret_cast<ConvertWorkItem*>(
        g_async_queue_pop(self->work_queue));

    if (item->shutdown) {
      delete item;
      break;
    }

    FlMethodResponse* response;
    if (item->is_bytes) {
      response = do_convert_to_bytes_work(item->input_path,
                                           item->compression_level,
                                           item->preserve_metadata);
    } else {
      response = do_convert_work(item->input_path, item->output_path,
                                  item->compression_level,
                                  item->preserve_metadata);
    }

    auto* delivery = new ResultDelivery();
    delivery->method_call = item->method_call;  // already ref'd
    delivery->response = FL_METHOD_RESPONSE(g_object_ref(response));
    delivery->plugin = HEIC_NATIVE_PLUGIN(g_object_ref(data));
    g_object_unref(response);
    g_idle_add(deliver_result_idle, delivery);

    g_free(item->input_path);
    g_free(item->output_path);
    delete item;
  }

  return nullptr;
}

// Called when a method call is received from Flutter.
static void heic_native_plugin_handle_method_call(HeicNativePlugin* self,
                                               FlMethodCall* method_call) {
  const gchar* method = fl_method_call_get_name(method_call);
  FlValue* args = fl_method_call_get_args(method_call);

  if (strcmp(method, "convert") == 0) {
    if (!args || fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
      g_autoptr(FlMethodResponse) response =
          FL_METHOD_RESPONSE(fl_method_error_response_new(
              "invalid_arguments", "Invalid arguments", nullptr));
      fl_method_call_respond(method_call, response, nullptr);
      return;
    }

    FlValue* input_val = fl_value_lookup_string(args, "inputPath");
    FlValue* output_val = fl_value_lookup_string(args, "outputPath");
    if (!input_val || !output_val ||
        fl_value_get_type(input_val) != FL_VALUE_TYPE_STRING ||
        fl_value_get_type(output_val) != FL_VALUE_TYPE_STRING) {
      g_autoptr(FlMethodResponse) response =
          FL_METHOD_RESPONSE(fl_method_error_response_new(
              "invalid_arguments", "inputPath and outputPath are required", nullptr));
      fl_method_call_respond(method_call, response, nullptr);
      return;
    }

    int compression_level = 6;
    FlValue* level_val = fl_value_lookup_string(args, "compressionLevel");
    if (level_val && fl_value_get_type(level_val) == FL_VALUE_TYPE_INT) {
      compression_level = static_cast<int>(fl_value_get_int(level_val));
      if (compression_level < 0) compression_level = 0;
      if (compression_level > 9) compression_level = 9;
    }

    bool preserve_metadata = true;
    FlValue* meta_val = fl_value_lookup_string(args, "preserveMetadata");
    if (meta_val && fl_value_get_type(meta_val) == FL_VALUE_TYPE_BOOL) {
      preserve_metadata = fl_value_get_bool(meta_val);
    }

    auto* item = new ConvertWorkItem{};
    item->input_path = g_strdup(fl_value_get_string(input_val));
    item->output_path = g_strdup(fl_value_get_string(output_val));
    item->compression_level = compression_level;
    item->preserve_metadata = preserve_metadata;
    item->is_bytes = false;
    item->method_call = FL_METHOD_CALL(g_object_ref(method_call));
    item->shutdown = false;
    g_async_queue_push(self->work_queue, item);

  } else if (strcmp(method, "convertToBytes") == 0) {
    if (!args || fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
      g_autoptr(FlMethodResponse) response =
          FL_METHOD_RESPONSE(fl_method_error_response_new(
              "invalid_arguments", "Invalid arguments", nullptr));
      fl_method_call_respond(method_call, response, nullptr);
      return;
    }

    FlValue* input_val = fl_value_lookup_string(args, "inputPath");
    if (!input_val ||
        fl_value_get_type(input_val) != FL_VALUE_TYPE_STRING) {
      g_autoptr(FlMethodResponse) response =
          FL_METHOD_RESPONSE(fl_method_error_response_new(
              "invalid_arguments", "inputPath is required", nullptr));
      fl_method_call_respond(method_call, response, nullptr);
      return;
    }

    int compression_level = 6;
    FlValue* level_val = fl_value_lookup_string(args, "compressionLevel");
    if (level_val && fl_value_get_type(level_val) == FL_VALUE_TYPE_INT) {
      compression_level = static_cast<int>(fl_value_get_int(level_val));
      if (compression_level < 0) compression_level = 0;
      if (compression_level > 9) compression_level = 9;
    }

    bool preserve_metadata = true;
    FlValue* meta_val = fl_value_lookup_string(args, "preserveMetadata");
    if (meta_val && fl_value_get_type(meta_val) == FL_VALUE_TYPE_BOOL) {
      preserve_metadata = fl_value_get_bool(meta_val);
    }

    auto* item = new ConvertWorkItem{};
    item->input_path = g_strdup(fl_value_get_string(input_val));
    item->output_path = nullptr;
    item->compression_level = compression_level;
    item->preserve_metadata = preserve_metadata;
    item->is_bytes = true;
    item->method_call = FL_METHOD_CALL(g_object_ref(method_call));
    item->shutdown = false;
    g_async_queue_push(self->work_queue, item);

  } else {
    g_autoptr(FlMethodResponse) response =
        FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
    fl_method_call_respond(method_call, response, nullptr);
  }
}

static void heic_native_plugin_dispose(GObject* object) {
  HeicNativePlugin* self = HEIC_NATIVE_PLUGIN(object);
  self->disposed = TRUE;

  // Push shutdown sentinel
  auto* sentinel = new ConvertWorkItem{};
  sentinel->shutdown = true;
  sentinel->input_path = nullptr;
  sentinel->output_path = nullptr;
  sentinel->method_call = nullptr;
  g_async_queue_push(self->work_queue, sentinel);

  g_thread_join(self->worker_thread);
  g_async_queue_unref(self->work_queue);

  G_OBJECT_CLASS(heic_native_plugin_parent_class)->dispose(object);
}

static void heic_native_plugin_class_init(HeicNativePluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = heic_native_plugin_dispose;
}

static void heic_native_plugin_init(HeicNativePlugin* self) {
  self->disposed = FALSE;
  self->work_queue = g_async_queue_new();
  self->worker_thread = g_thread_new("heic_native-worker", worker_thread_func, self);
}

static void method_call_cb(FlMethodChannel* /*channel*/,
                           FlMethodCall* method_call, gpointer user_data) {
  HeicNativePlugin* plugin = HEIC_NATIVE_PLUGIN(user_data);
  heic_native_plugin_handle_method_call(plugin, method_call);
}

void heic_native_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  // Load libheif's codec plugins. On its own, libheif only registers the
  // codecs compiled into it; decoders built as external plugins, which is how
  // Debian and Ubuntu package libheif, are loaded by heif_init(). Without this
  // call, a build that ends up on such a libheif parses HEIC containers fine
  // but cannot decode a single image. The Windows plugin has always called it.
  // There is deliberately no matching heif_deinit(): libheif must not be torn
  // down during process exit (see the Windows plugin for the details).
  heif_error init_err = heif_init(nullptr);
  if (init_err.code != heif_error_Ok) {
    g_warning("heic_native: heif_init failed: %s",
              init_err.message != nullptr ? init_err.message : "unknown error");
  }

  HeicNativePlugin* plugin = HEIC_NATIVE_PLUGIN(
      g_object_new(heic_native_plugin_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel =
      fl_method_channel_new(fl_plugin_registrar_get_messenger(registrar),
                            "heic_native", FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(channel, method_call_cb,
                                            g_object_ref(plugin),
                                            g_object_unref);

  g_object_unref(plugin);
}
