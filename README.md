# OpenMedia

**Open Media codecs abstraction** - A modern, modular multimedia framework with a clean C++ API.

## Overview

OpenMedia is a next-generation multimedia library designed to provide decoding, encoding, and demuxing capabilities for
audio, video, and image formats. Built with modern C++20, it offers a flexible plugin-style architecture for codec
integration while maintaining a simple, intuitive API.

> **Note:** Not all codecs currently support encoding yet and is currently a work in progress.
> The library currently focuses on decoding and demuxing.

### Key Features

- Audio Codecs: AAC, ALAC, FLAC, Opus, Vorbis, MP3, AAC, PCM, and more
- Video Codecs: AV1, H.264/AVC, H.265/HEVC, H.266/VVC, EVC, VP8, VP9, and more
- Image Formats: PNG, JPEG, WebP, GIF, BMP, TIFF, TGA
- Container Support - Matroska (MKV/MKA/WebM), MP4, Ogg, Wav, and more
- Modular Architecture - Enable/disable codecs at compile-time via CMake options
- Hardware Acceleration - Support for Vulkan Video, DirectX 11/12 Video, VA-API
- Modern C++20 API - Clean, type-safe interfaces, exception-free, rtti-free
- Example Player - SDL3-based reference player

---

## Implemented Codecs

### Audio Codecs

**Status:** ✅ Implemented | 🔧 Planned

| Codec   | Decoding | Encoding | Backends                |
|---------|:--------:|:--------:|-------------------------|
| AAC     |    ✅     |    🔧    | libfdk-aac, WMF, FFmpeg |
| ALAC    |    ✅     |    🔧    | libalac                 |
| FLAC    |    ✅     |    🔧    | libFLAC                 |
| Opus    |    ✅     |    ✅     | libopus                 |
| Vorbis  |    ✅     |    🔧    | libvorbis               |
| MP3     |    ✅     |    🔧    | minimp3                 |
| WAV/PCM |    ✅     |    🔧    | OpenMedia               |

### Video Codecs

**Status:** ✅ Implemented | 🔧 Planned

| Codec     | Decoding | Encoding | Backends                      |
|-----------|:--------:|:--------:|-------------------------------|
| AV1       |    ✅     |    🔧    | dav1d (decoding only)         |
| H264      |    ✅     |    🔧    | OpenH264, FFmpeg              |
| H265/HEVC |    ✅     |    🔧    | FFmpeg                        |
| H266/VVC  |    ✅     |    🔧    | FFmpeg, VVdeC (Broken), VVenC |
| EVC       | Untested | Untested | FFmpeg, xevd, xeve            |
| VP8/VP9   | Untested |    🔧    | FFmpeg, libvpx                |

### Image Codecs

**Status:** ✅ Implemented | 🔧 Planned

| Codec | Decoding |  Encoding   | Backends                                      |
|-------|:--------:|:-----------:|-----------------------------------------------|
| PNG   |    ✅     |     🔧      | Portable Network Graphics (decoder & demuxer) |
| JPEG  |    ✅     |     🔧      | Joint Photographic Experts Group              |
| WebP  |    ✅     |     🔧      | Modern image format by Google                 |
| GIF   |    ✅     | Not Planned | Graphics Interchange Format                   |
| BMP   |    ✅     |     🔧      | Bitmap image format                           |
| TIFF  |    ✅     |     🔧      | Tagged Image File Format                      |
| TGA   |    ✅     |     🔧      | Truevision TARGA                              |
| EXR   |    🔧    |     🔧      | OpenEXR                                       |

---

### Hardware Acceleration

OpenMedia provides interfaces for hardware-accelerated decoding and encoding:

**Status:** ✅ Implemented | 🔧 Planned

| API              | Status | Platform      | 
|------------------|:------:|---------------|
| VideoToolbox     |   🔧   | macOS         |
| VA-API           |   🔧   | Linux         |
| AMF              |   🔧   | Windows       |
| Vulkan Video     |   🔧   | Windows/Linux |
| DirectX 11 Video |   🔧   | Windows       |
| DirectX 12 Video |   🔧   | Windows       |
| CUDA/NVDEC       |   🔧   | Windows/Linux |
| NVENC            |   🔧   | Windows/Linux |
| Intel® Media SDK |   🔧   | Windows       |
| MediaCodec       |   🔧   | Android       |

---

### Container Formats

**Status:** ✅ Implemented | 🔧 Planned

| Format                  | Demuxing | Muxing      | Description                         |
|-------------------------|:--------:|-------------|-------------------------------------|
| Matroska (MKV/MKA/WebM) |    ✅     | Untested    | Matroska container (libwebm)        |
| WebM                    |    ✅     | Untested    | Google's web media format (libwebm) |
| MP4/MOV (BMFF)          |    ✅     | 🔧          | ISO Base Media File Format          |
| MOV/QuickTime           |    🔧    | 🔧          | Apple QuickTime format              |
| Ogg                     |    ✅     | 🔧          | Ogg container                       |
| WAV                     |    ✅     | 🔧          | WAV container                       |
| FLAC                    |    ✅     | 🔧          | FLAC container                      |
| MP3                     |    ✅     | 🔧          | MP3 container                       |
| AVI                     |    🔧    | Not planned | Audio Video Interleave              |

---

## Building

### Requirements

- CMake 3.21+
- C++20 compatible compiler (Clang, MSVC, GCC)
- Optional: gitdeps

### Example Build

```bash
mkdir build && cd build
cmake .. -DOPENMEDIA_EXAMPLE_PLAYER=ON
cmake --build .
```

---

## Usage Example

```cpp
#include <openmedia/codec_api.hpp>
#include <openmedia/format_detector.hpp>
#include <openmedia/codec_registry.hpp>
#include <openmedia/format_registry.hpp>

using namespace openmedia;

int main() {
    // Initialize registries
    CodecRegistry codec_registry;
    FormatRegistry format_registry;
    FormatDetector format_detector;
    
    // Register built-in codecs and formats
    registerBuiltInCodecs(&codec_registry);
    registerBuiltInFormats(&format_registry);
    format_detector.addAllStandard();
    
    // Create decoder for a specific codec
    auto decoder = codec_registry.createDecoder(OM_CODEC_AV1);
    if (decoder) {
        DecoderOptions options;
        options.format = /* ... */;
        decoder->configure(options);
        
        auto result = decoder->decode(packet);
        if (result.isOk()) {
            auto frames = result.unwrap();
            // Process decoded frames...
        }
    }
    
    return 0;
}
```

---

## License

OpenMedia is provided under the terms of its respective license. See [LICENSE](LICENSE) for details.

---

## Contributing

Contributions are welcome! Whether it's adding new codecs, improving existing implementations, or fixing bugs, please
feel free to submit issues and pull requests.

---
