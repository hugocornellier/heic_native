#include "heic_native_plugin.h"

// Prevent windows.h from defining min/max macros that conflict with std::min/std::max.
#define NOMINMAX

// This must be included before many other Windows headers.
#include <windows.h>

#include <commctrl.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <random>
#include <vector>

#include <libheif/heif.h>
#include <png.h>

namespace heic_native {

namespace {

std::wstring Utf8ToWide(const std::string &str) {
  if (str.empty()) return std::wstring();
  int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
                                 static_cast<int>(str.size()), nullptr, 0);
  if (size == 0) return std::wstring();
  std::wstring result(size, 0);
  MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()),
                      &result[0], size);
  return result;
}

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

// Read a file into memory using the wide-path API for full Unicode support.
static bool read_file_to_memory(const std::wstring &path,
                                std::vector<uint8_t> &out) {
  FILE *f = _wfopen(path.c_str(), L"rb");
  if (!f) return false;
  _fseeki64(f, 0, SEEK_END);
  long long file_size = _ftelli64(f);
  _fseeki64(f, 0, SEEK_SET);
  if (file_size <= 0 || file_size > (1LL << 30)) {  // 1 GB max
    fclose(f);
    return false;
  }
  out.resize(static_cast<size_t>(file_size));
  size_t read = fread(out.data(), 1, out.size(), f);
  fclose(f);
  return (read == out.size());
}

// Decode a HEIC image from an in-memory buffer into raw RGBA pixels.
// Returns true on success; caller must release *out_img, *out_handle, *out_ctx.
static bool decode_heic(const std::vector<uint8_t> &file_data,
                        heif_context **out_ctx,
                        heif_image_handle **out_handle,
                        heif_image **out_img,
                        int *out_width,
                        int *out_height,
                        int *out_stride,
                        const uint8_t **out_pixels,
                        bool *out_has_alpha) {
  heif_context *ctx = heif_context_alloc();
  if (!ctx) return false;
  heif_context_set_max_decoding_threads(ctx, 0);

  heif_error err = heif_context_read_from_memory(
      ctx, file_data.data(), file_data.size(), nullptr);
  if (err.code != heif_error_Ok) {
    heif_context_free(ctx);
    return false;
  }

  heif_image_handle *handle = nullptr;
  err = heif_context_get_primary_image_handle(ctx, &handle);
  if (err.code != heif_error_Ok) {
    heif_context_free(ctx);
    return false;
  }

  bool has_alpha = heif_image_handle_has_alpha_channel(handle);

  heif_image *img = nullptr;
  err = heif_decode_image(handle, &img, heif_colorspace_RGB,
                          has_alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB,
                          nullptr);
  if (err.code != heif_error_Ok) {
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return false;
  }

  int stride = 0;
  const uint8_t *pixels =
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
static bool normalize_exif_orientation(std::vector<uint8_t> &exif_data) {
  if (exif_data.size() < 8) return false;

  // Skip the 4-byte header offset that libheif prepends.
  uint32_t hdr_offset =
      (static_cast<uint32_t>(exif_data[0]) << 24) |
      (static_cast<uint32_t>(exif_data[1]) << 16) |
      (static_cast<uint32_t>(exif_data[2]) << 8) |
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
             (static_cast<uint32_t>(exif_data[off + 1]) << 8) |
             (static_cast<uint32_t>(exif_data[off + 2]) << 16) |
             (static_cast<uint32_t>(exif_data[off + 3]) << 24);
    return (static_cast<uint32_t>(exif_data[off]) << 24) |
           (static_cast<uint32_t>(exif_data[off + 1]) << 16) |
           (static_cast<uint32_t>(exif_data[off + 2]) << 8) |
           static_cast<uint32_t>(exif_data[off + 3]);
  };

  auto write16 = [&](size_t off, uint16_t val) {
    if (off + 2 > exif_data.size()) return;
    if (little_endian) {
      exif_data[off] = static_cast<uint8_t>(val & 0xFF);
      exif_data[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    } else {
      exif_data[off] = static_cast<uint8_t>((val >> 8) & 0xFF);
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
      write16(entry_pos + 8, 1);  // 1 = Normal
      return true;
    }
  }
  return false;
}

// Patch EXIF dimension tags to match the decoded (possibly rotated) image size.
// Updates IFD0 tags 0x0100/0x0101 and ExifIFD tags 0xA002/0xA003.
static void update_exif_dimensions(std::vector<uint8_t> &exif_data, int width, int height) {
  if (exif_data.size() < 8) return;

  uint32_t hdr_offset =
      (static_cast<uint32_t>(exif_data[0]) << 24) |
      (static_cast<uint32_t>(exif_data[1]) << 16) |
      (static_cast<uint32_t>(exif_data[2]) << 8) |
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
             (static_cast<uint32_t>(exif_data[off + 1]) << 8) |
             (static_cast<uint32_t>(exif_data[off + 2]) << 16) |
             (static_cast<uint32_t>(exif_data[off + 3]) << 24);
    return (static_cast<uint32_t>(exif_data[off]) << 24) |
           (static_cast<uint32_t>(exif_data[off + 1]) << 16) |
           (static_cast<uint32_t>(exif_data[off + 2]) << 8) |
           static_cast<uint32_t>(exif_data[off + 3]);
  };

  auto write16 = [&](size_t off, uint16_t val) {
    if (off + 2 > exif_data.size()) return;
    if (little_endian) {
      exif_data[off] = static_cast<uint8_t>(val & 0xFF);
      exif_data[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    } else {
      exif_data[off] = static_cast<uint8_t>((val >> 8) & 0xFF);
      exif_data[off + 1] = static_cast<uint8_t>(val & 0xFF);
    }
  };

  auto write32 = [&](size_t off, uint32_t val) {
    if (off + 4 > exif_data.size()) return;
    if (little_endian) {
      exif_data[off] = static_cast<uint8_t>(val & 0xFF);
      exif_data[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
      exif_data[off + 2] = static_cast<uint8_t>((val >> 16) & 0xFF);
      exif_data[off + 3] = static_cast<uint8_t>((val >> 24) & 0xFF);
    } else {
      exif_data[off] = static_cast<uint8_t>((val >> 24) & 0xFF);
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
static HeicMetadata extract_metadata(heif_image_handle *handle, int width, int height) {
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

// Set ICC and EXIF metadata on a PNG being written.
static void set_png_metadata(png_structp png_ptr, png_infop info_ptr,
                             const HeicMetadata *metadata) {
  if (!metadata) return;

  if (!metadata->icc_profile.empty()) {
    png_set_iCCP(png_ptr, info_ptr, "icc",
                 PNG_COMPRESSION_TYPE_BASE,
                 const_cast<png_bytep>(metadata->icc_profile.data()),
                 static_cast<png_uint_32>(metadata->icc_profile.size()));
  }

#if PNG_LIBPNG_VER >= 10632
  if (metadata->exif_data.size() > 4) {
    uint32_t tiff_offset =
        (static_cast<uint32_t>(metadata->exif_data[0]) << 24) |
        (static_cast<uint32_t>(metadata->exif_data[1]) << 16) |
        (static_cast<uint32_t>(metadata->exif_data[2]) << 8) |
        static_cast<uint32_t>(metadata->exif_data[3]);
    if (tiff_offset <= metadata->exif_data.size() - 4) {
      size_t skip = 4 + static_cast<size_t>(tiff_offset);
      png_set_eXIf_1(
          png_ptr, info_ptr,
          static_cast<png_uint_32>(metadata->exif_data.size() - skip),
          const_cast<png_bytep>(metadata->exif_data.data() + skip));
    }
  }
#endif
}

// Write RGBA pixels as a PNG into a memory buffer.
// Returns true on success.
static bool write_png_to_memory(std::vector<uint8_t> *buf, int width,
                                int height, int stride,
                                const uint8_t *pixels, int compression_level,
                                const HeicMetadata *metadata, bool has_alpha) {
  png_structp png_ptr =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png_ptr) return false;

  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    png_destroy_write_struct(&png_ptr, nullptr);
    return false;
  }

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

  set_png_metadata(png_ptr, info_ptr, metadata);
  png_write_info(png_ptr, info_ptr);
  if (state.error) png_error(png_ptr, "memory allocation failed");

  for (int y = 0; y < height; y++) {
    png_write_row(
        png_ptr,
        const_cast<png_bytep>(pixels + static_cast<size_t>(y) * stride));
    if (state.error) png_error(png_ptr, "memory allocation failed");
  }

  png_write_end(png_ptr, nullptr);
  if (state.error) png_error(png_ptr, "memory allocation failed");
  png_destroy_write_struct(&png_ptr, &info_ptr);
  return true;
}

}  // namespace

// static
void HeicNativePlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows *registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "heic_native",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<HeicNativePlugin>();

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto &call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  auto *plugin_ptr = plugin.get();
  registrar->AddPlugin(std::move(plugin));
  auto *view = registrar->GetView();
  if (view) {
    plugin_ptr->flutter_window_ = view->GetNativeWindow();
    if (plugin_ptr->flutter_window_) {
      SetWindowSubclass(plugin_ptr->flutter_window_,
                        HeicNativePlugin::ResultSubclassProc,
                        reinterpret_cast<UINT_PTR>(plugin_ptr),
                        reinterpret_cast<DWORD_PTR>(plugin_ptr));
    }
  }
}

HeicNativePlugin::HeicNativePlugin()
    : worker_thread_(&HeicNativePlugin::WorkerLoop, this) {
  heif_init(nullptr);
}

HeicNativePlugin::~HeicNativePlugin() {
  // Shut down worker thread first, it may be mid-conversion using libheif.
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    shutting_down_ = true;
  }
  queue_cv_.notify_one();
  if (worker_thread_.joinable()) worker_thread_.join();

  // Note: we intentionally do NOT call heif_deinit() here.
  // libheif docs warn: "heif_deinit() must not be called after exit(),
  // for example in a global C++ object's destructor. If you do, global
  // variables in libheif might have already been released when
  // heif_deinit() is running, leading to a crash."
  // Since this destructor runs during DLL_PROCESS_DETACH / static
  // destruction, calling heif_deinit() causes access violations on
  // Windows. The OS reclaims all memory on process exit anyway.

  // RemoveWindowSubclass is handled by WM_NCDESTROY in ResultSubclassProc.
  // If the window is still alive (unusual), remove it defensively.
  if (flutter_window_) {
    RemoveWindowSubclass(flutter_window_, ResultSubclassProc,
                         reinterpret_cast<UINT_PTR>(this));
    flutter_window_ = nullptr;
  }
}

void HeicNativePlugin::WorkerLoop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] {
        return shutting_down_.load() || !work_queue_.empty();
      });
      if (shutting_down_.load() && work_queue_.empty()) break;
      task = std::move(work_queue_.front());
      work_queue_.pop();
    }
    task();
  }
}

void HeicNativePlugin::PostResultToMainThread(std::function<void()> cb) {
  if (!flutter_window_ || shutting_down_.load()) {
    // No window or shutting down, drop the callback silently.
    return;
  }
  {
    std::lock_guard<std::mutex> lock(result_mutex_);
    result_queue_.push(std::move(cb));
  }
  PostMessage(flutter_window_, WM_HEIC_NATIVE_RESULT, 0, 0);
}

LRESULT CALLBACK HeicNativePlugin::ResultSubclassProc(HWND hwnd, UINT msg,
                                                     WPARAM wp, LPARAM lp,
                                                     UINT_PTR id,
                                                     DWORD_PTR ref) {
  if (msg == WM_HEIC_NATIVE_RESULT) {
    auto *p = reinterpret_cast<HeicNativePlugin *>(ref);
    std::function<void()> cb;
    {
      std::lock_guard<std::mutex> lock(p->result_mutex_);
      if (!p->result_queue_.empty()) {
        cb = std::move(p->result_queue_.front());
        p->result_queue_.pop();
      }
    }
    if (cb) cb();
    return 0;
  }
  if (msg == WM_NCDESTROY) {
    // Window is being destroyed, stop using this HWND.
    auto *p = reinterpret_cast<HeicNativePlugin *>(ref);
    RemoveWindowSubclass(hwnd, ResultSubclassProc, id);
    p->flutter_window_ = nullptr;
    // Don't return 0, let DefSubclassProc handle WM_NCDESTROY.
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}

void HeicNativePlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const std::string &method = method_call.method_name();

  if (method == "convert") {
    const auto *args =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!args) {
      result->Error("invalid_arguments",
                    "inputPath and outputPath are required");
      return;
    }
    auto input_it = args->find(flutter::EncodableValue("inputPath"));
    auto output_it = args->find(flutter::EncodableValue("outputPath"));
    if (input_it == args->end() || output_it == args->end()) {
      result->Error("invalid_arguments",
                    "inputPath and outputPath are required");
      return;
    }
    const std::string *input_path =
        std::get_if<std::string>(&input_it->second);
    const std::string *output_path =
        std::get_if<std::string>(&output_it->second);
    if (!input_path || !output_path) {
      result->Error("invalid_arguments",
                    "inputPath and outputPath are required");
      return;
    }

    int compression_level = 6;
    auto level_it = args->find(flutter::EncodableValue("compressionLevel"));
    if (level_it != args->end()) {
      const auto *level = std::get_if<int32_t>(&level_it->second);
      if (level) compression_level = std::max(0, std::min(9, *level));
    }

    bool preserve_metadata = true;
    auto meta_it = args->find(flutter::EncodableValue("preserveMetadata"));
    if (meta_it != args->end()) {
      const auto *meta = std::get_if<bool>(&meta_it->second);
      if (meta) preserve_metadata = *meta;
    }

    auto shared_result =
        std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>(
            std::move(result));
    std::string in = *input_path;
    std::string out = *output_path;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      work_queue_.push([this, in, out, compression_level, preserve_metadata,
                        shared_result]() {
        std::string error_code, error_message;
        bool success = ConvertHeicToPng(in, out, compression_level,
                                        preserve_metadata, error_code,
                                        error_message);
        PostResultToMainThread(
            [shared_result, success, error_code, error_message]() {
              if (success) {
                shared_result->Success(flutter::EncodableValue(true));
              } else {
                shared_result->Error(error_code, error_message);
              }
            });
      });
    }
    queue_cv_.notify_one();

  } else if (method == "convertToBytes") {
    const auto *args =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (!args) {
      result->Error("invalid_arguments", "inputPath is required");
      return;
    }
    auto input_it = args->find(flutter::EncodableValue("inputPath"));
    if (input_it == args->end()) {
      result->Error("invalid_arguments", "inputPath is required");
      return;
    }
    const std::string *input_path =
        std::get_if<std::string>(&input_it->second);
    if (!input_path) {
      result->Error("invalid_arguments", "inputPath is required");
      return;
    }

    int compression_level = 6;
    auto level_it = args->find(flutter::EncodableValue("compressionLevel"));
    if (level_it != args->end()) {
      const auto *level = std::get_if<int32_t>(&level_it->second);
      if (level) compression_level = std::max(0, std::min(9, *level));
    }

    bool preserve_metadata = true;
    auto meta_it = args->find(flutter::EncodableValue("preserveMetadata"));
    if (meta_it != args->end()) {
      const auto *meta = std::get_if<bool>(&meta_it->second);
      if (meta) preserve_metadata = *meta;
    }

    auto shared_result =
        std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>(
            std::move(result));
    std::string in = *input_path;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      work_queue_.push([this, in, compression_level, preserve_metadata,
                        shared_result]() {
        std::string error_code, error_message;
        std::vector<uint8_t> bytes = ConvertHeicToPngBytes(
            in, compression_level, preserve_metadata, error_code,
            error_message);
        PostResultToMainThread(
            [shared_result, bytes = std::move(bytes), error_code,
             error_message]() {
              if (bytes.empty()) {
                shared_result->Error(error_code, error_message);
              } else {
                shared_result->Success(flutter::EncodableValue(bytes));
              }
            });
      });
    }
    queue_cv_.notify_one();

  } else {
    result->NotImplemented();
  }
}

bool HeicNativePlugin::ConvertHeicToPng(const std::string &input_path,
                                      const std::string &output_path,
                                      int compression_level,
                                      bool preserve_metadata,
                                      std::string &error_code,
                                      std::string &error_message) {
  // Encode to PNG in memory first (avoids passing a FILE* into libpng16.dll
  // across the DLL boundary, which can cause access violations on Windows when
  // the process has been running long enough for heap/CRT state to diverge
  // between heic_native_plugin.dll and libpng16.dll).
  std::vector<uint8_t> png_bytes = ConvertHeicToPngBytes(
      input_path, compression_level, preserve_metadata,
      error_code, error_message);
  if (png_bytes.empty()) return false;

  // Write bytes to a temp file alongside the output, then atomically rename.
  std::wstring w_output = Utf8ToWide(output_path);
  std::random_device rd;
  uint64_t r = (static_cast<uint64_t>(rd()) << 32) | rd();
  wchar_t suffix[32];
  swprintf(suffix, 32, L".%016llx.heic_native.tmp", r);
  std::wstring w_temp = w_output + suffix;

  FILE *fp = _wfopen(w_temp.c_str(), L"wb");
  if (!fp) {
    error_code = "encode_failed";
    error_message = "Failed to open output file for writing";
    return false;
  }
  size_t written = fwrite(png_bytes.data(), 1, png_bytes.size(), fp);
  fclose(fp);

  if (written != png_bytes.size()) {
    DeleteFileW(w_temp.c_str());
    error_code = "encode_failed";
    error_message = "Failed to write PNG file";
    return false;
  }

  if (!MoveFileExW(w_temp.c_str(), w_output.c_str(),
                   MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(w_temp.c_str());
    error_code = "encode_failed";
    error_message = "Failed to move output file";
    return false;
  }

  return true;
}

std::vector<uint8_t> HeicNativePlugin::ConvertHeicToPngBytes(
    const std::string &input_path, int compression_level,
    bool preserve_metadata, std::string &error_code,
    std::string &error_message) {
  std::wstring w_input = Utf8ToWide(input_path);

  // Read the HEIC file into memory.
  std::vector<uint8_t> file_data;
  if (!read_file_to_memory(w_input, file_data)) {
    error_code = "decode_failed";
    error_message = "Failed to read HEIC file";
    return {};
  }

  // Decode HEIC to raw RGBA pixels.
  heif_context *ctx = nullptr;
  heif_image_handle *handle = nullptr;
  heif_image *img = nullptr;
  int width = 0, height = 0, stride = 0;
  const uint8_t *pixels = nullptr;
  bool has_alpha = false;

  if (!decode_heic(file_data, &ctx, &handle, &img, &width, &height, &stride,
                   &pixels, &has_alpha)) {
    error_code = "decode_failed";
    error_message = "Failed to decode HEIC file";
    return {};
  }

  HeicMetadata meta;
  if (preserve_metadata) {
    meta = extract_metadata(handle, width, height);
  }

  // Encode to PNG in memory.
  std::vector<uint8_t> result;
  bool success = write_png_to_memory(&result, width, height, stride, pixels,
                                     compression_level,
                                     preserve_metadata ? &meta : nullptr,
                                     has_alpha);

  heif_image_release(img);
  heif_image_handle_release(handle);
  heif_context_free(ctx);

  if (!success || result.empty()) {
    error_code = "encode_failed";
    error_message = "Failed to encode PNG";
    return {};
  }

  return result;
}

}  // namespace heic_native
