# OpenMedia

**Open FFmpeg alternative** - A modern, modular multimedia framework with a clean C++ API.

## Overview

OpenMedia is a next-generation multimedia library designed to provide decoding, encoding, and demuxing capabilities for
audio, video, and image formats. Built with modern C++20, it offers a flexible plugin-style architecture for codec
integration while maintaining a simple, intuitive API.

> **Note:** Encoding is not yet supported and is currently a work in progress. The library currently focuses on
> decoding and demuxing.

### Key Features

- Modern C++20 API - Clean, type-safe interfaces with error handling via `Result<T, E>`
- Exception-Free - No C++ exceptions used; error handling via `Result<T, E>` and error codes
- RTTI-Free - No runtime type information; uses variant-based polymorphism
- Modular Architecture - Enable/disable codecs at compile-time via CMake options
- Hardware Acceleration - Support for Vulkan Video, DirectX 11/12 Video, VA-API
- Container Support - Matroska (MKV/MKA/WebM), MP4, Ogg, and more
- Audio Codecs - FLAC, ALAC, Opus, Vorbis, MP3, AAC, PCM, and more
- Video Codecs - AV1 (dav1d), H.264, H.265/HEVC, VP9, and more
- Image Formats - PNG, JPEG, WebP, GIF, BMP, TIFF, TGA, EXR
- Example Player - SDL3-based reference player

---

## Implemented Codecs

### Audio Codecs

**Status:** ✅ Implemented

| Codec                    | Status | Description                                   |
|--------------------------|:------:|-----------------------------------------------|
| FLAC                     |   ✅    | Free Lossless Audio Codec (decoder & demuxer) |
| ALAC                     |   ✅    | Apple Lossless Audio Codec                    |
| Opus                     |   ✅    | Interactive audio codec by Xiph.Org           |
| Vorbis                   |   ✅    | Open audio codec by Xiph.Org                  |
| MP3                      |   ✅    | MPEG-1/2 Audio Layer III (decoder & demuxer)  |
| OGG                      |   ✅    | Ogg container demuxer                         |
| WAV/PCM                  |   ✅    | Uncompressed audio (various bit depths)       |
| Windows Media Foundation |   ✅    | Windows native codec support                  |

### Video Codecs

**Status:** ✅ Implemented | 🔧 Planned

| Codec                           | Status | Description                             |
|---------------------------------|:------:|-----------------------------------------|
| dav1d (AV1)                     |   ✅    | High-efficiency AV1 decoder by VideoLAN |
| OpenH264                        |   🔧   | H.264/AVC codec by Cisco                |
| DirectX11/12 Video (H264, H265) |   🔧   | Hardware Coding                         |
| Vulkan Video (H264, H265)       |   🔧   | Hardware Coding                         |
| VVdeC (H.266/VVC)               |   🔧   | Versatile Video Coding                  |
| VVenC (H.266/VVC)               |   🔧   | Versatile Video Coding                  |
| xevd (EVC)                      |   🔧   | Essential Video Coding                  |
| xeve (EVC)                      |   🔧   | Essential Video Coding                  |
| libvpx (VP8/VP9)                |   🔧   | Google's open video codecs              |

### Image Codecs

**Status:** ✅ Implemented | 🔧 Planned

| Codec | Status | Description                                   |
|-------|:------:|-----------------------------------------------|
| PNG   |   ✅    | Portable Network Graphics (decoder & demuxer) |
| JPEG  |   ✅    | Joint Photographic Experts Group              |
| WebP  |   ✅    | Modern image format by Google                 |
| GIF   |   ✅    | Graphics Interchange Format                   |
| BMP   |   ✅    | Bitmap image format                           |
| TIFF  |   ✅    | Tagged Image File Format                      |
| TGA   |   ✅    | Truevision TARGA                              |
| EXR   |   🔧   | OpenEXR high dynamic range                    |

---

## Hardware Acceleration

OpenMedia provides interfaces for hardware-accelerated decoding and encoding:

**Status:** ✅ Implemented | 🔧 Planned

| API              | Status | Platform      | Description                                |
|------------------|:------:|---------------|--------------------------------------------|
| Vulkan Video     |   ✅    | Windows/Linux | Khronos standard for GPU-accelerated video |
| VA-API           |   🔧   | Linux         | Video Acceleration API (Intel/AMD GPUs)    |
| DirectX 11 Video |   🔧   | Windows       | D3D11 video decode/encode                  |
| DirectX 12 Video |   🔧   | Windows       | D3D12 video decode/encode                  |
| CUDA/NVDEC       |   🔧   | Windows/Linux | NVIDIA hardware decoding                   |
| NVENC            |   🔧   | Windows/Linux | NVIDIA hardware encoding (NV Codec SDK)    |
| AMF              |   🔧   | Windows/Linux | AMD Advanced Media Framework               |
| MediaCodec       |   ✅    | Android       | Android hardware codec API                 |

---

## Planned Codecs & Integrations

### Third-Party Codec Integrations

**Status:** ✅ Implemented | 🔧 Planned

| Codec/Framework                 | Status | Description                                              |
|---------------------------------|:------:|----------------------------------------------------------|
| FFmpeg (libavcodec/libavformat) |   ✅    | Optional FFmpeg backend for comprehensive format support |
| AMF (AMD)                       |   🔧   | H.264, H.265, AV1 encoders/decoders for AMD GPUs         |
| OpenH264                        |   🔧   | H.264 baseline codec                                     |
| NVENC/NVDEC                     |   🔧   | NVIDIA hardware codecs via NV Codec SDK                  |
| Intel Media SDK                 |   🔧   | Intel Quick Sync Video (QSV)                             |
| libx264/libx265                 |   🔧   | Software H.264/H.265 encoders                            |
| SVT-AV1/SVT-VP9                 |   🔧   | Scalable Video Technology encoders                       |
| libvpx                          |   🔧   | VP8/VP9 reference codec                                  |

### Container Formats

**Status:** ✅ Implemented | 🔧 Planned

| Format                  | Status | Description                              |
|-------------------------|:------:|------------------------------------------|
| Matroska (MKV/MKA/WebM) |   ✅    | Full demuxer support via libwebm         |
| MP4/MOV (BMFF)          |   ✅    | ISO Base Media File Format demuxer       |
| Ogg                     |   ✅    | Ogg container demuxer                    |
| WAV                     |   ✅    | WAV demuxer                              |
| FLAC                    |   ✅    | Native FLAC demuxer                      |
| MP3                     |   ✅    | MP3 demuxer                              |
| AVI                     |   🔧   | Audio Video Interleave                   |
| WebM                    |   ✅    | Google's web media format (via Matroska) |
| MOV/QuickTime           |   🔧   | Apple QuickTime format                   |

---

## Building

### Requirements

- CMake 3.21+
- C++20 compatible compiler (Clang, MSVC, GCC)
- Optional: AUI Boot for automatic dependency management

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
