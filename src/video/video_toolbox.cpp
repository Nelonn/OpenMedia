#include <codecs.hpp>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <openmedia/video.hpp>
#include <span>
#include <vector>

namespace openmedia {

namespace {

constexpr int32_t kDefaultTimeScale = 1'000'000;

template<typename T>
class CFPtr {
public:
  CFPtr() = default;

  explicit CFPtr(T ref)
      : ref_(ref) {}

  CFPtr(const CFPtr&) = delete;
  auto operator=(const CFPtr&) -> CFPtr& = delete;

  CFPtr(CFPtr&& other) noexcept
      : ref_(other.ref_) {
    other.ref_ = nullptr;
  }

  auto operator=(CFPtr&& other) noexcept -> CFPtr& {
    if (this != &other) {
      reset(other.ref_);
      other.ref_ = nullptr;
    }
    return *this;
  }

  ~CFPtr() {
    reset();
  }

  auto get() const noexcept -> T { return ref_; }
  explicit operator bool() const noexcept { return ref_ != nullptr; }

  void reset(T ref = nullptr) noexcept {
    if (ref_) {
      CFRelease(ref_);
    }
    ref_ = ref;
  }

private:
  T ref_ = nullptr;
};

auto codecIdToVideoToolboxFormat(OMCodecId codec_id, OMProfile profile) noexcept -> CMVideoCodecType {
  switch (codec_id) {
    case OM_CODEC_H263:
      return kCMVideoCodecType_H263;
    case OM_CODEC_H264:
      return kCMVideoCodecType_H264;
    case OM_CODEC_H265:
      return kCMVideoCodecType_HEVC;
    case OM_CODEC_H262:
      return kCMVideoCodecType_MPEG2Video;
    case OM_CODEC_MPEG4:
      return kCMVideoCodecType_MPEG4Video;
    case OM_CODEC_AV1:
      return kCMVideoCodecType_AV1;
    case OM_CODEC_PRORES:
      switch (profile) {
        case OM_PROFILE_PRORES_PROXY: return kCMVideoCodecType_AppleProRes422Proxy;
        case OM_PROFILE_PRORES_LT: return kCMVideoCodecType_AppleProRes422LT;
        case OM_PROFILE_PRORES_STANDARD: return kCMVideoCodecType_AppleProRes422;
        case OM_PROFILE_PRORES_HQ: return kCMVideoCodecType_AppleProRes422HQ;
        case OM_PROFILE_PRORES_4444: return kCMVideoCodecType_AppleProRes4444;
        case OM_PROFILE_PRORES_XQ: return kCMVideoCodecType_AppleProRes4444XQ;
        default: return kCMVideoCodecType_AppleProRes422;
      }
    default:
      return 0;
  }
}

auto isParameterSetCodec(OMCodecId codec_id) noexcept -> bool {
  return codec_id == OM_CODEC_H264 || codec_id == OM_CODEC_H265;
}

auto isAnnexB(std::span<const uint8_t> bytes) noexcept -> bool {
  if (bytes.size() < 3) return false;
  if (bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0x01) return true;
  if (bytes.size() >= 4 && bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0x00 && bytes[3] == 0x01) return true;
  return false;
}

auto nextStartCode(std::span<const uint8_t> bytes, size_t offset) noexcept -> size_t {
  for (size_t i = offset; i + 3 <= bytes.size(); ++i) {
    if (bytes[i] == 0x00 && bytes[i + 1] == 0x00) {
      if (bytes[i + 2] == 0x01) {
        return i;
      }
      if (i + 4 <= bytes.size() && bytes[i + 2] == 0x00 && bytes[i + 3] == 0x01) {
        return i;
      }
    }
  }
  return bytes.size();
}

auto startCodeLength(std::span<const uint8_t> bytes, size_t offset) noexcept -> size_t {
  if (offset + 3 <= bytes.size() &&
      bytes[offset] == 0x00 &&
      bytes[offset + 1] == 0x00 &&
      bytes[offset + 2] == 0x01) {
    return 3;
  }
  if (offset + 4 <= bytes.size() &&
      bytes[offset] == 0x00 &&
      bytes[offset + 1] == 0x00 &&
      bytes[offset + 2] == 0x00 &&
      bytes[offset + 3] == 0x01) {
    return 4;
  }
  return 0;
}

template<typename Fn>
void forEachAnnexBNal(std::span<const uint8_t> bytes, Fn&& fn) {
  size_t start = nextStartCode(bytes, 0);
  while (start < bytes.size()) {
    const size_t prefix = startCodeLength(bytes, start);
    if (prefix == 0) {
      break;
    }

    const size_t nal_start = start + prefix;
    size_t next = nextStartCode(bytes, nal_start);
    
    if (next > nal_start) {
      fn(bytes.subspan(nal_start, next - nal_start));
    }
    start = next;
  }
}

auto annexBToLengthPrefixed(std::span<const uint8_t> bytes, size_t length_size = 4) -> std::vector<uint8_t> {
  std::vector<uint8_t> out;
  out.reserve(bytes.size());

  forEachAnnexBNal(bytes, [&](std::span<const uint8_t> nal) {
    const auto size = static_cast<uint32_t>(nal.size());
    for (size_t i = 0; i < length_size; ++i) {
      const size_t shift = 8 * (length_size - i - 1);
      out.push_back(static_cast<uint8_t>((size >> shift) & 0xFF));
    }
    out.insert(out.end(), nal.begin(), nal.end());
  });

  return out;
}

auto h264NalType(std::span<const uint8_t> nal) noexcept -> uint8_t {
  return nal.empty() ? 0 : (nal[0] & 0x1Fu);
}

auto hevcNalType(std::span<const uint8_t> nal) noexcept -> uint8_t {
  return nal.empty() ? 0 : ((nal[0] >> 1u) & 0x3Fu);
}

struct ParameterSets {
  std::vector<std::vector<uint8_t>> sets;
  int nal_length_size = 4;
};

auto isAVCC(std::span<const uint8_t> bytes) noexcept -> bool {
  if (bytes.size() < 7) return false;
  return bytes[0] == 1; // version
}

auto parseH264ParameterSets(std::span<const uint8_t> extradata) -> ParameterSets {
  ParameterSets result;

  if (isAVCC(extradata)) {
    result.nal_length_size = (extradata[4] & 0x03u) + 1;
    size_t offset = 5;
    const uint8_t sps_count = extradata[offset++] & 0x1Fu;
    for (uint8_t i = 0; i < sps_count && offset + 2 <= extradata.size(); ++i) {
      const size_t size = (static_cast<size_t>(extradata[offset]) << 8u) | extradata[offset + 1];
      offset += 2;
      if (offset + size > extradata.size()) break;
      result.sets.emplace_back(extradata.begin() + offset, extradata.begin() + offset + size);
      offset += size;
    }
    if (offset < extradata.size()) {
      const uint8_t pps_count = extradata[offset++];
      for (uint8_t i = 0; i < pps_count && offset + 2 <= extradata.size(); ++i) {
        const size_t size = (static_cast<size_t>(extradata[offset]) << 8u) | extradata[offset + 1];
        offset += 2;
        if (offset + size > extradata.size()) break;
        result.sets.emplace_back(extradata.begin() + offset, extradata.begin() + offset + size);
        offset += size;
      }
    }
  } else if (isAnnexB(extradata)) {
    forEachAnnexBNal(extradata, [&](std::span<const uint8_t> nal) {
      const uint8_t type = h264NalType(nal);
      if (type == 7 || type == 8 || type == 13) {
        result.sets.emplace_back(nal.begin(), nal.end());
      }
    });
  }

  return result;
}

auto parseHEVCParameterSets(std::span<const uint8_t> extradata) -> ParameterSets {
  ParameterSets result;

  if (extradata.size() >= 23 && extradata[0] == 1) { // HVCC
    result.nal_length_size = (extradata[21] & 0x03u) + 1;
    size_t offset = 22;
    const uint8_t array_count = extradata[offset++];
    for (uint8_t i = 0; i < array_count && offset + 3 <= extradata.size(); ++i) {
      offset += 1; // nal_unit_type
      const uint16_t nal_count = static_cast<uint16_t>((extradata[offset] << 8u) | extradata[offset + 1]);
      offset += 2;
      for (uint16_t j = 0; j < nal_count && offset + 2 <= extradata.size(); ++j) {
        const size_t size = (static_cast<size_t>(extradata[offset]) << 8u) | extradata[offset + 1];
        offset += 2;
        if (offset + size > extradata.size()) break;
        result.sets.emplace_back(extradata.begin() + offset, extradata.begin() + offset + size);
        offset += size;
      }
    }
  } else if (isAnnexB(extradata)) {
    forEachAnnexBNal(extradata, [&](std::span<const uint8_t> nal) {
      const uint8_t type = hevcNalType(nal);
      if (type == 32 || type == 33 || type == 34 || type == 39 || type == 40) {
        result.sets.emplace_back(nal.begin(), nal.end());
      }
    });
  }

  return result;
}

auto makeExtradataExtensions(OMCodecId codec_id, std::span<const uint8_t> extradata) -> CFPtr<CFDictionaryRef> {
  if (extradata.empty()) {
    return {};
  }

  const char* atom = nullptr;
  switch (codec_id) {
    case OM_CODEC_AV1: atom = "av1C"; break;
    case OM_CODEC_MPEG4: atom = "esds"; break;
    default: break;
  }
  if (!atom) {
    return {};
  }

  CFPtr<CFStringRef> atom_key(CFStringCreateWithCString(kCFAllocatorDefault, atom, kCFStringEncodingASCII));
  CFPtr<CFDataRef> atom_data(CFDataCreate(kCFAllocatorDefault,
                                          reinterpret_cast<const UInt8*>(extradata.data()),
                                          static_cast<CFIndex>(extradata.size())));
  if (!atom_key || !atom_data) {
    return {};
  }

  CFTypeRef atom_keys[] = {atom_key.get()};
  CFTypeRef atom_values[] = {atom_data.get()};
  CFPtr<CFDictionaryRef> atoms(CFDictionaryCreate(kCFAllocatorDefault,
                                                  atom_keys,
                                                  atom_values,
                                                  1,
                                                  &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks));
  if (!atoms) {
    return {};
  }

  CFTypeRef extension_keys[] = {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms};
  CFTypeRef extension_values[] = {atoms.get()};
  return CFPtr<CFDictionaryRef>(CFDictionaryCreate(kCFAllocatorDefault,
                                                   extension_keys,
                                                   extension_values,
                                                   1,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks));
}

auto createFormatDescription(OMCodecId codec_id,
                             OMProfile profile,
                             uint32_t width,
                             uint32_t height,
                             std::span<const uint8_t> extradata,
                             int* nal_length_size) -> CFPtr<CMVideoFormatDescriptionRef> {
  if (nal_length_size) {
    *nal_length_size = 4;
  }

  CMFormatDescriptionRef raw_parameter_format = nullptr;
  if (codec_id == OM_CODEC_H264) {
    auto parameter_sets = parseH264ParameterSets(extradata);
    if (nal_length_size) {
      *nal_length_size = parameter_sets.nal_length_size;
    }
    if (parameter_sets.sets.size() >= 2) {
      std::vector<const uint8_t*> pointers;
      std::vector<size_t> sizes;
      pointers.reserve(parameter_sets.sets.size());
      sizes.reserve(parameter_sets.sets.size());
      for (const auto& set : parameter_sets.sets) {
        pointers.push_back(set.data());
        sizes.push_back(set.size());
      }
      OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault,
                                                                            pointers.size(),
                                                                            pointers.data(),
                                                                            sizes.data(),
                                                                            parameter_sets.nal_length_size,
                                                                            &raw_parameter_format);
      if (status == noErr && raw_parameter_format) {
        log(OM_CATEGORY_DECODER, OM_LEVEL_INFO, std::format("VideoToolbox H.264 format description created successfully, nal_length_size={}", parameter_sets.nal_length_size));
        return CFPtr<CMVideoFormatDescriptionRef>(static_cast<CMVideoFormatDescriptionRef>(raw_parameter_format));
      }
      log(OM_CATEGORY_DECODER,
          OM_LEVEL_ERROR,
          std::format("VideoToolbox H.264 parameter-set format creation failed: {}", static_cast<int32_t>(status)));
    } else {
        log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, std::format("VideoToolbox H.264: Not enough parameter sets found ({})", parameter_sets.sets.size()));
    }
  }

  if (codec_id == OM_CODEC_H265) {
    auto parameter_sets = parseHEVCParameterSets(extradata);
    if (nal_length_size) {
      *nal_length_size = parameter_sets.nal_length_size;
    }
    if (parameter_sets.sets.size() >= 3) {
      std::vector<const uint8_t*> pointers;
      std::vector<size_t> sizes;
      pointers.reserve(parameter_sets.sets.size());
      sizes.reserve(parameter_sets.sets.size());
      for (const auto& set : parameter_sets.sets) {
        pointers.push_back(set.data());
        sizes.push_back(set.size());
      }
      OSStatus status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(kCFAllocatorDefault,
                                                                            pointers.size(),
                                                                            pointers.data(),
                                                                            sizes.data(),
                                                                            parameter_sets.nal_length_size,
                                                                            nullptr,
                                                                            &raw_parameter_format);
      if (status == noErr && raw_parameter_format) {
        log(OM_CATEGORY_DECODER, OM_LEVEL_INFO, std::format("VideoToolbox HEVC format description created successfully, nal_length_size={}", parameter_sets.nal_length_size));
        return CFPtr<CMVideoFormatDescriptionRef>(static_cast<CMVideoFormatDescriptionRef>(raw_parameter_format));
      }
      log(OM_CATEGORY_DECODER,
          OM_LEVEL_ERROR,
          std::format("VideoToolbox HEVC parameter-set format creation failed: {}", static_cast<int32_t>(status)));
    } else {
        log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, std::format("VideoToolbox HEVC: Not enough parameter sets found ({})", parameter_sets.sets.size()));
    }
  }

  CMVideoFormatDescriptionRef raw_format = nullptr;
  auto extensions = makeExtradataExtensions(codec_id, extradata);
  OSStatus status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault,
                                                   codecIdToVideoToolboxFormat(codec_id, profile),
                                                   static_cast<int32_t>(width),
                                                   static_cast<int32_t>(height),
                                                   extensions.get(),
                                                   &raw_format);
  if (status != noErr || !raw_format) {
    log(OM_CATEGORY_DECODER,
        OM_LEVEL_ERROR,
        std::format("VideoToolbox format creation failed: {}", static_cast<int32_t>(status)));
    return {};
  }

  return CFPtr<CMVideoFormatDescriptionRef>(raw_format);
}

auto makeOutputAttributes() -> CFPtr<CFDictionaryRef> {
  int32_t formats[] = {
    kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
    kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
    kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
    kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
  };
  CFPtr<CFNumberRef> f8v(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &formats[0]));
  CFPtr<CFNumberRef> f8f(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &formats[1]));
  CFPtr<CFNumberRef> f10v(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &formats[2]));
  CFPtr<CFNumberRef> f10f(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &formats[3]));
  
  const void* values[] = { f8v.get(), f8f.get(), f10v.get(), f10f.get() };
  CFPtr<CFArrayRef> formats_array(CFArrayCreate(kCFAllocatorDefault, values, 4, &kCFTypeArrayCallBacks));
  if (!formats_array) {
    return {};
  }

  CFTypeRef keys[] = {kCVPixelBufferPixelFormatTypeKey};
  CFTypeRef array_val[] = {formats_array.get()};
  return CFPtr<CFDictionaryRef>(CFDictionaryCreate(kCFAllocatorDefault,
                                                   keys,
                                                   array_val,
                                                   1,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks));
}

auto makeDecoderSpecification() -> CFPtr<CFDictionaryRef> {
  CFTypeRef keys[] = {kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder};
  CFTypeRef values[] = {kCFBooleanTrue};
  return CFPtr<CFDictionaryRef>(CFDictionaryCreate(kCFAllocatorDefault,
                                                   keys,
                                                   values,
                                                   1,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks));
}

auto makeTime(int64_t value, int32_t timescale) -> CMTime {
  return value >= 0 ? CMTimeMake(value, timescale) : kCMTimeInvalid;
}

void copyPlaneWithStride(uint8_t* dst,
                         size_t dst_stride,
                         const uint8_t* src,
                         size_t src_stride,
                         size_t row_bytes,
                         size_t height) {
  for (size_t y = 0; y < height; ++y) {
    std::memcpy(dst, src, row_bytes);
    dst += dst_stride;
    src += src_stride;
  }
}

auto cvPixelFormatToOpenMedia(OSType pixel_format) noexcept -> OMPixelFormat {
  switch (pixel_format) {
    case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
    case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
      return OM_FORMAT_NV12;
    case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
    case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
      return OM_FORMAT_P010;
    case kCVPixelFormatType_32BGRA:
      return OM_FORMAT_B8G8R8A8;
    default:
      return OM_FORMAT_UNKNOWN;
  }
}

struct FrameTiming {
  int64_t pts = -1;
  int64_t dts = -1;
  bool is_keyframe = false;
};

class VideoToolboxDecoder final : public Decoder {
public:
  explicit VideoToolboxDecoder(OMCodecId codec_id)
      : codec_id_(codec_id) {}

  ~VideoToolboxDecoder() override {
    closeSession();
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    closeSession();

    if (options.format.type != OM_MEDIA_VIDEO || options.format.codec_id != codec_id_) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.format.video.width == 0 || options.format.video.height == 0) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (codecIdToVideoToolboxFormat(codec_id_, options.format.profile) == 0) {
      return OM_CODEC_NOT_SUPPORTED;
    }

    profile_ = options.format.profile;
    color_space_ = options.format.video.color_space;
    transfer_char_ = options.format.video.transfer_char;
    output_format_.width = options.format.video.width;
    output_format_.height = options.format.video.height;
    bool is_10_bit = (profile_ == OM_PROFILE_H265_MAIN_10 ||
                      profile_ == OM_PROFILE_H265_MAIN_4_2_2_10 ||
                      profile_ == OM_PROFILE_H265_MAIN_4_4_4_10 ||
                      profile_ == OM_PROFILE_AV1_HIGH ||
                      profile_ == OM_PROFILE_AV1_PROFESSIONAL);
    bool is_12_bit = (profile_ == OM_PROFILE_H265_MAIN_12 ||
                      profile_ == OM_PROFILE_H265_MAIN_4_2_2_12 ||
                      profile_ == OM_PROFILE_H265_MAIN_4_4_4_12);
    bool is_16_bit = (profile_ == OM_PROFILE_H265_REXT);

    if (const auto* val = options.extra.get("pixel_format")) {
      if (val->isString()) {
        auto str = val->getString().value();
        if (str == "P010" || str == "p010") { is_10_bit = true; is_12_bit = false; is_16_bit = false; }
        else if (str == "P012" || str == "p012") { is_12_bit = true; is_10_bit = false; is_16_bit = false; }
        else if (str == "P016" || str == "p016") { is_16_bit = true; is_10_bit = false; is_12_bit = false; }
      } else if (val->isInt32()) {
        auto fmt = static_cast<OMPixelFormat>(val->getInt32().value());
        if (fmt == OM_FORMAT_P010) { is_10_bit = true; is_12_bit = false; is_16_bit = false; }
        else if (fmt == OM_FORMAT_P012) { is_12_bit = true; is_10_bit = false; is_16_bit = false; }
        else if (fmt == OM_FORMAT_P016) { is_16_bit = true; is_10_bit = false; is_12_bit = false; }
      }
    }

    if (is_16_bit) {
      output_format_.format = OM_FORMAT_P016;
    } else if (is_12_bit) {
      output_format_.format = OM_FORMAT_P012;
    } else if (is_10_bit) {
      output_format_.format = OM_FORMAT_P010;
    } else {
      output_format_.format = OM_FORMAT_NV12;
    }
    timescale_ = (options.time_base.num > 0 && options.time_base.den > 0)
                     ? options.time_base.den
                     : kDefaultTimeScale;

    format_description_ = createFormatDescription(codec_id_,
                                                  profile_,
                                                  output_format_.width,
                                                  output_format_.height,
                                                  options.extradata,
                                                  &nal_length_size_);
    if (!format_description_) {
      return OM_CODEC_OPEN_FAILED;
    }

    auto err = openSession();
    if (err != OM_SUCCESS) {
      return err;
    }

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) {
      return std::nullopt;
    }

    DecodingInfo info = {};
    info.media_type = OM_MEDIA_VIDEO;
    info.video_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (!initialized_ || !session_) {
      return Err(OM_COMMON_NOT_INITIALIZED);
    }

    if (packet.bytes.empty()) {
      VTDecompressionSessionFinishDelayedFrames(session_.get());
      VTDecompressionSessionWaitForAsynchronousFrames(session_.get());
      return Ok(collectFrames());
    }

    std::span<const uint8_t> payload(packet.bytes.data(), packet.bytes.size());
    std::vector<uint8_t> converted;
    if (isParameterSetCodec(codec_id_) && isAnnexB(payload)) {
      converted = annexBToLengthPrefixed(payload, 4);
      if (!converted.empty()) {
        payload = converted;
      }
    }

    auto block_buffer = makeBlockBuffer(payload);
    if (!block_buffer) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    CMSampleTimingInfo timing = {};
    timing.duration = packet.duration > 0 ? CMTimeMake(packet.duration, timescale_) : kCMTimeInvalid;
    timing.presentationTimeStamp = makeTime(packet.pts, timescale_);
    timing.decodeTimeStamp = makeTime(packet.dts, timescale_);

    const size_t sample_size = payload.size();
    CMSampleBufferRef raw_sample_buffer = nullptr;
    OSStatus status = CMSampleBufferCreateReady(kCFAllocatorDefault,
                                                block_buffer.get(),
                                                format_description_.get(),
                                                1,
                                                1,
                                                &timing,
                                                1,
                                                &sample_size,
                                                &raw_sample_buffer);
    CFPtr<CMSampleBufferRef> sample_buffer(raw_sample_buffer);
    if (status != noErr || !sample_buffer) {
      logStatus(OM_LEVEL_ERROR, "CMSampleBufferCreateReady", status);
      return Err(OM_CODEC_DECODE_FAILED);
    }

    if (auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample_buffer.get(), true)) {
      auto attachment = const_cast<CFMutableDictionaryRef>(
          static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0)));
      if (attachment) {
        if (!packet.is_keyframe) {
          CFDictionarySetValue(attachment, kCMSampleAttachmentKey_NotSync, kCFBooleanTrue);
        }
        CFDictionarySetValue(attachment,
                             kCMSampleAttachmentKey_DependsOnOthers,
                             packet.is_keyframe ? kCFBooleanFalse : kCFBooleanTrue);
      }
    }

    auto* frame_timing = new FrameTiming();
    frame_timing->pts = packet.pts;
    frame_timing->dts = packet.dts;
    frame_timing->is_keyframe = packet.is_keyframe;

    VTDecodeInfoFlags info_flags = 0;
    status = VTDecompressionSessionDecodeFrame(session_.get(),
                                               sample_buffer.get(),
                                               kVTDecodeFrame_EnableAsynchronousDecompression,
                                               frame_timing,
                                               &info_flags);
    if (status != noErr) {
      logStatus(OM_LEVEL_ERROR, "VTDecompressionSessionDecodeFrame", status);
      delete frame_timing;
    }

    return Ok(collectFrames());
  }

  void flush() override {
    if (session_) {
      VTDecompressionSessionFinishDelayedFrames(session_.get());
      VTDecompressionSessionWaitForAsynchronousFrames(session_.get());
    }
    std::lock_guard<std::mutex> lock(frames_mutex_);
    decoded_frames_.clear();
  }

  void handleOutput(OSStatus status, CVImageBufferRef image_buffer, FrameTiming* timing, CMTime pts, CMTime duration) {
    std::unique_ptr<FrameTiming> timing_ptr(timing);
    if (status != noErr || !image_buffer) {
      if (status != noErr) {
        log(OM_CATEGORY_DECODER,
            OM_LEVEL_ERROR,
            std::format("VideoToolbox decode callback failed: {}", static_cast<int32_t>(status)));
      }
      return;
    }

    auto pixel_buffer = static_cast<CVPixelBufferRef>(image_buffer);
    const OSType cv_format = CVPixelBufferGetPixelFormatType(pixel_buffer);
    const OMPixelFormat cv_om_format = cvPixelFormatToOpenMedia(cv_format);
    if (cv_om_format == OM_FORMAT_UNKNOWN) {
      return;
    }

    CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
    const auto width = static_cast<uint32_t>(CVPixelBufferGetWidth(pixel_buffer));
    const auto height = static_cast<uint32_t>(CVPixelBufferGetHeight(pixel_buffer));
    
    OMPixelFormat om_format = cv_om_format;
    if (cv_om_format == OM_FORMAT_P010 && (output_format_.format == OM_FORMAT_P012 || output_format_.format == OM_FORMAT_P016)) {
      om_format = output_format_.format;
    }
    output_format_.format = om_format;

    Picture picture(om_format, width, height);
    picture.color_space = color_space_;
    picture.transfer_char = transfer_char_;
    picture.color_range = OM_COLOR_RANGE_MPEG;
    if (cv_format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
        cv_format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange) {
        picture.color_range = OM_COLOR_RANGE_JPEG;
    }
    picture.is_keyframe = timing_ptr ? timing_ptr->is_keyframe : false;

    if (CVPixelBufferIsPlanar(pixel_buffer)) {
      const size_t plane_count = CVPixelBufferGetPlaneCount(pixel_buffer);
      for (size_t i = 0; i < plane_count && i < 2; ++i) {
        const auto* src = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, i));
        const size_t src_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, i);
        const size_t plane_height = CVPixelBufferGetHeightOfPlane(pixel_buffer, i);
        const size_t plane_width = CVPixelBufferGetWidthOfPlane(pixel_buffer, i);
        size_t bpp = getBytesPerPixel(om_format, static_cast<uint8_t>(i));
        if (i == 1) {
            if (om_format == OM_FORMAT_NV12) bpp = 2;
            else if (om_format == OM_FORMAT_P010 || om_format == OM_FORMAT_P012 || om_format == OM_FORMAT_P016) bpp = 4;
        }
        copyPlaneWithStride(picture.planes.data[i], picture.planes.linesize[i], src, src_stride, plane_width * bpp, plane_height);
      }
    } else {
      const auto* src = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixel_buffer));
      const size_t src_stride = CVPixelBufferGetBytesPerRow(pixel_buffer);
      copyPlaneWithStride(picture.planes.data[0], picture.planes.linesize[0], src, src_stride, width * getBytesPerPixel(om_format, 0), height);
    }
    CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);

    Frame frame = {};
    if (CMTIME_IS_VALID(pts)) {
        frame.pts = static_cast<uint64_t>(CMTimeGetSeconds(pts) * timescale_);
    } else {
        frame.pts = timing_ptr && timing_ptr->pts >= 0 ? static_cast<uint64_t>(timing_ptr->pts) : 0;
    }
    frame.dts = frame.pts; // VideoToolbox handles reordering, so we return frames in presentation order
    frame.data = std::move(picture);

    std::lock_guard<std::mutex> lock(frames_mutex_);
    decoded_frames_.push_back(std::move(frame));
  }

private:
  auto openSession() -> OMError {
    auto output_attributes = makeOutputAttributes();
    auto decoder_specification = makeDecoderSpecification();
    VTDecompressionOutputCallbackRecord callback = {};
    callback.decompressionOutputCallback = [](void* refcon, void* source, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef img, CMTime pts, CMTime dur) {
      static_cast<VideoToolboxDecoder*>(refcon)->handleOutput(status, img, static_cast<FrameTiming*>(source), pts, dur);
    };
    callback.decompressionOutputRefCon = this;

    VTDecompressionSessionRef raw_session = nullptr;
    OSStatus status = VTDecompressionSessionCreate(kCFAllocatorDefault,
                                                   format_description_.get(),
                                                   decoder_specification.get(),
                                                   output_attributes.get(),
                                                   &callback,
                                                   &raw_session);
    if (status != noErr || !raw_session) {
      logStatus(OM_LEVEL_ERROR, "VTDecompressionSessionCreate", status);
      return OM_CODEC_OPEN_FAILED;
    }

    session_.reset(raw_session);
    return OM_SUCCESS;
  }

  void closeSession() {
    initialized_ = false;
    if (session_) {
      VTDecompressionSessionInvalidate(session_.get());
      session_.reset();
    }
    format_description_.reset();
    output_format_ = {};
    profile_ = OM_PROFILE_NONE;
    nal_length_size_ = 4;
    timescale_ = kDefaultTimeScale;
    std::lock_guard<std::mutex> lock(frames_mutex_);
    decoded_frames_.clear();
  }

  auto collectFrames() -> std::vector<Frame> {
    std::lock_guard<std::mutex> lock(frames_mutex_);
    return std::move(decoded_frames_);
  }

  auto makeBlockBuffer(std::span<const uint8_t> payload) const -> CFPtr<CMBlockBufferRef> {
    CMBlockBufferRef raw_block_buffer = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault,
                                                         nullptr,
                                                         payload.size(),
                                                         kCFAllocatorDefault,
                                                         nullptr,
                                                         0,
                                                         payload.size(),
                                                         0,
                                                         &raw_block_buffer);
    CFPtr<CMBlockBufferRef> block_buffer(raw_block_buffer);
    if (status != noErr || !block_buffer) {
      logStatus(OM_LEVEL_ERROR, "CMBlockBufferCreateWithMemoryBlock", status);
      return {};
    }

    status = CMBlockBufferReplaceDataBytes(payload.data(), block_buffer.get(), 0, payload.size());
    if (status != noErr) {
      logStatus(OM_LEVEL_ERROR, "CMBlockBufferReplaceDataBytes", status);
      return {};
    }

    return block_buffer;
  }

  void logStatus(OMLogLevel level, std::string_view operation, OSStatus status) const {
    log(OM_CATEGORY_DECODER,
        level,
        std::format("VideoToolbox {} failed: {}", operation, static_cast<int32_t>(status)));
  }

  OMCodecId codec_id_ = OM_CODEC_NONE;
  OMProfile profile_ = OM_PROFILE_NONE;
  CFPtr<CMVideoFormatDescriptionRef> format_description_;
  CFPtr<VTDecompressionSessionRef> session_;
  VideoFormat output_format_ = {};
  OMColorSpace color_space_ = OM_COLOR_SPACE_BT709;
  OMTransferCharacteristic transfer_char_ = OM_TRANSFER_BT709;
  int nal_length_size_ = 4;
  int32_t timescale_ = kDefaultTimeScale;
  bool initialized_ = false;

  std::mutex frames_mutex_;
  std::vector<Frame> decoded_frames_;
};

auto h264CMSampleBufferToAnnexB(CMSampleBufferRef avcc_sample_buffer, bool is_keyframe, std::vector<uint8_t>& annexb_buffer) -> bool {
  CMVideoFormatDescriptionRef description = CMSampleBufferGetFormatDescription(avcc_sample_buffer);
  if (description == nullptr) {
    return false;
  }

  OSStatus code;

  int nalu_header_size = 0;
  size_t param_set_count = 0;
  code = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(description, 0, nullptr, nullptr, &param_set_count, &nalu_header_size);
  if (code != noErr) {
    return false;
  }

  if (is_keyframe) {
    size_t param_set_size = 0;
    const uint8_t* param_set = nullptr;
    for (size_t i = 0; i < param_set_count; ++i) {
      code = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
          description, i, &param_set, &param_set_size, nullptr, nullptr);
      if (code != noErr) {
        return false;
      }
      annexb_buffer.insert(annexb_buffer.end(), {0, 0, 0, 1});
      annexb_buffer.insert(annexb_buffer.end(), param_set, param_set + param_set_size);
    }
  }

  CMBlockBufferRef block_buffer = CMSampleBufferGetDataBuffer(avcc_sample_buffer);
  if (block_buffer == nullptr) {
    return false;
  }
  
  size_t block_buffer_size = CMBlockBufferGetDataLength(block_buffer);
  std::vector<uint8_t> tmp(block_buffer_size);
  code = CMBlockBufferCopyDataBytes(block_buffer, 0, block_buffer_size, tmp.data());
  if (code != noErr) {
    return false;
  }

  size_t offset = 0;
  while (offset + nalu_header_size <= block_buffer_size) {
    uint32_t nalu_size = 0;
    for (int i = 0; i < nalu_header_size; ++i) {
        nalu_size = (nalu_size << 8) | tmp[offset + i];
    }
    offset += nalu_header_size;
    if (offset + nalu_size > block_buffer_size) break;

    annexb_buffer.insert(annexb_buffer.end(), {0, 0, 0, 1});
    annexb_buffer.insert(annexb_buffer.end(), tmp.data() + offset, tmp.data() + offset + nalu_size);
    offset += nalu_size;
  }

  return true;
}

auto hevcCMSampleBufferToAnnexB(CMSampleBufferRef hvcc_sample_buffer, bool is_keyframe, std::vector<uint8_t>& annexb_buffer) -> bool {
  CMVideoFormatDescriptionRef description = CMSampleBufferGetFormatDescription(hvcc_sample_buffer);
  if (description == nullptr) {
    return false;
  }

  OSStatus code;

  int nalu_header_size = 0;
  size_t param_set_count = 0;
  code = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(description, 0, nullptr, nullptr, &param_set_count, &nalu_header_size);
  if (code != noErr) {
    return false;
  }

  if (is_keyframe) {
    size_t param_set_size = 0;
    const uint8_t* param_set = nullptr;
    for (size_t i = 0; i < param_set_count; ++i) {
      code = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
          description, i, &param_set, &param_set_size, nullptr, nullptr);
      if (code != noErr) {
        return false;
      }
      annexb_buffer.insert(annexb_buffer.end(), {0, 0, 0, 1});
      annexb_buffer.insert(annexb_buffer.end(), param_set, param_set + param_set_size);
    }
  }

  CMBlockBufferRef block_buffer = CMSampleBufferGetDataBuffer(hvcc_sample_buffer);
  if (block_buffer == nullptr) {
    return false;
  }
  
  size_t block_buffer_size = CMBlockBufferGetDataLength(block_buffer);
  std::vector<uint8_t> tmp(block_buffer_size);
  code = CMBlockBufferCopyDataBytes(block_buffer, 0, block_buffer_size, tmp.data());
  if (code != noErr) {
    return false;
  }

  size_t offset = 0;
  while (offset + nalu_header_size <= block_buffer_size) {
    uint32_t nalu_size = 0;
    for (int i = 0; i < nalu_header_size; ++i) {
        nalu_size = (nalu_size << 8) | tmp[offset + i];
    }
    offset += nalu_header_size;
    if (offset + nalu_size > block_buffer_size) break;

    annexb_buffer.insert(annexb_buffer.end(), {0, 0, 0, 1});
    annexb_buffer.insert(annexb_buffer.end(), tmp.data() + offset, tmp.data() + offset + nalu_size);
    offset += nalu_size;
  }

  return true;
}

struct EncoderCallbackContext {
  std::mutex mutex;
  std::vector<Packet> packets;
  std::vector<uint8_t> extradata;
  OMCodecId codec_id;
  int32_t timescale;
};

void compressionCallback(void* compression_output_ref_con,
                         void* source_frame_ref_con,
                         OSStatus status,
                         VTEncodeInfoFlags info_flags,
                         CMSampleBufferRef sample_buffer) {
  auto* context = static_cast<EncoderCallbackContext*>(compression_output_ref_con);
  if (!context || status != noErr || !sample_buffer) {
    return;
  }

  {
      std::lock_guard<std::mutex> lock(context->mutex);
      if (context->extradata.empty()) {
          CMVideoFormatDescriptionRef desc = CMSampleBufferGetFormatDescription(sample_buffer);
          if (desc) {
              CFDictionaryRef extensions = CMFormatDescriptionGetExtensions(desc);
              if (extensions) {
                  CFDataRef atom = static_cast<CFDataRef>(CFDictionaryGetValue(extensions, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms));
                  if (atom) {
                      const uint8_t* data = CFDataGetBytePtr(atom);
                      CFIndex size = CFDataGetLength(atom);
                      context->extradata.assign(data, data + size);
                  }
              }
          }
      }
  }

  bool is_keyframe = false;
  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample_buffer, false);
  if (attachments != nullptr && CFArrayGetCount(attachments)) {
    CFDictionaryRef attachment = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
    is_keyframe = !CFDictionaryContainsKey(attachment, kCMSampleAttachmentKey_NotSync);
  }

  std::vector<uint8_t> annexb;
  bool ok = false;
  if (context->codec_id == OM_CODEC_H264) {
      ok = h264CMSampleBufferToAnnexB(sample_buffer, is_keyframe, annexb);
  } else if (context->codec_id == OM_CODEC_H265) {
      ok = hevcCMSampleBufferToAnnexB(sample_buffer, is_keyframe, annexb);
  } else {
      // For ProRes, we can probably just use the raw data
      CMBlockBufferRef block_buffer = CMSampleBufferGetDataBuffer(sample_buffer);
      if (block_buffer) {
          size_t length = CMBlockBufferGetDataLength(block_buffer);
          annexb.resize(length);
          CMBlockBufferCopyDataBytes(block_buffer, 0, length, annexb.data());
          ok = true;
      }
  }

  if (ok) {
    Packet packet;
    packet.allocate(annexb.size());
    std::memcpy(packet.bytes.data(), annexb.data(), annexb.size());
    packet.pts = static_cast<int64_t>(CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample_buffer)) * context->timescale);
    packet.dts = static_cast<int64_t>(CMTimeGetSeconds(CMSampleBufferGetDecodeTimeStamp(sample_buffer)) * context->timescale);
    packet.is_keyframe = is_keyframe;
    std::lock_guard<std::mutex> lock(context->mutex);
    context->packets.push_back(std::move(packet));
  }
}

class VideoToolboxEncoder final : public Encoder {
public:
  explicit VideoToolboxEncoder(OMCodecId codec_id)
      : codec_id_(codec_id) {}

  ~VideoToolboxEncoder() override {
    closeSession();
  }

  auto configure(const EncoderOptions& options) -> OMError override {
    closeSession();

    if (options.format.type != OM_MEDIA_VIDEO || options.format.codec_id != codec_id_) {
      return OM_CODEC_INVALID_PARAMS;
    }

    width_ = options.format.video.width;
    height_ = options.format.video.height;
    timescale_ = kDefaultTimeScale;

    OSStatus status = VTCompressionSessionCreate(kCFAllocatorDefault,
                                                 static_cast<int32_t>(width_),
                                                 static_cast<int32_t>(height_),
                                                 codecIdToVideoToolboxFormat(codec_id_, options.format.profile),
                                                 nullptr,
                                                 nullptr,
                                                 nullptr,
                                                 compressionCallback,
                                                 &callback_context_,
                                                 &session_);
    if (status != noErr || !session_) {
        return OM_CODEC_OPEN_FAILED;
    }

    if (codec_id_ == OM_CODEC_H265) {
      bool is_high_bit_depth = (options.format.profile == OM_PROFILE_H265_MAIN_10 ||
                                options.format.profile == OM_PROFILE_H265_MAIN_4_2_2_10 ||
                                options.format.profile == OM_PROFILE_H265_MAIN_4_4_4_10 ||
                                options.format.profile == OM_PROFILE_H265_MAIN_12 ||
                                options.format.profile == OM_PROFILE_H265_MAIN_4_2_2_12 ||
                                options.format.profile == OM_PROFILE_H265_MAIN_4_4_4_12 ||
                                options.format.profile == OM_PROFILE_H265_REXT);
      if (const auto* val = options.extra.get("pixel_format")) {
        if (val->isString()) {
          auto str = val->getString().value();
          if (str == "P010" || str == "p010" || str == "P012" || str == "p012" || str == "P016" || str == "p016") {
            is_high_bit_depth = true;
          }
        } else if (val->isInt32()) {
          auto fmt = static_cast<OMPixelFormat>(val->getInt32().value());
          if (fmt == OM_FORMAT_P010 || fmt == OM_FORMAT_P012 || fmt == OM_FORMAT_P016) {
            is_high_bit_depth = true;
          }
        }
      }
      if (is_high_bit_depth) {
        OSStatus err = VTSessionSetProperty(session_, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main10_AutoLevel);
        if (err != noErr) {
          log(OM_CATEGORY_ENCODER, OM_LEVEL_WARNING, std::format("Failed to set HEVC Main 10 profile: {}", static_cast<int32_t>(err)));
        }
      }
    }

    callback_context_.codec_id = codec_id_;
    callback_context_.timescale = timescale_;

    updateBitrate(options.rate_control);

    VTCompressionSessionPrepareToEncodeFrames(session_);

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info = {};
    std::vector<uint8_t> extra;
    {
        std::lock_guard<std::mutex> lock(callback_context_.mutex);
        extra = callback_context_.extradata;
    }
    if (extradata_.empty() && !extra.empty()) {
        extradata_ = std::move(extra);
    }
    info.extradata = extradata_;
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_ || !session_) {
      return Err(OM_COMMON_NOT_INITIALIZED);
    }

    if (frame.data.valueless_by_exception()) {
        OSStatus status = VTCompressionSessionCompleteFrames(session_, kCMTimeInvalid);
        if (status != noErr) {
            return Err(OM_CODEC_ENCODE_FAILED);
        }
        std::vector<Packet> packets;
        {
            std::lock_guard<std::mutex> lock(callback_context_.mutex);
            packets = std::move(callback_context_.packets);
            callback_context_.packets.clear();
        }
        return Ok(std::move(packets));
    }

    const auto* picture = std::get_if<Picture>(&frame.data);
    if (!picture) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }

    OSType cv_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    if (picture->format == OM_FORMAT_NV12) {
        cv_format = (picture->color_range == OM_COLOR_RANGE_JPEG) ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    } else if (picture->format == OM_FORMAT_P010 || picture->format == OM_FORMAT_P012 || picture->format == OM_FORMAT_P016) {
        cv_format = (picture->color_range == OM_COLOR_RANGE_JPEG) ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
    }

    CVPixelBufferRef pixel_buffer = nullptr;
    OSStatus status = CVPixelBufferCreate(kCFAllocatorDefault,
                                          picture->width,
                                          picture->height,
                                          cv_format,
                                          nullptr,
                                          &pixel_buffer);
    if (status != noErr || !pixel_buffer) {
        return Err(OM_CODEC_ENCODE_FAILED);
    }

    CVPixelBufferLockBaseAddress(pixel_buffer, 0);
    for (uint32_t i = 0; i < picture->planes.count && i < 2; ++i) {
        uint8_t* dst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, i));
        size_t dst_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, i);
        auto dims = picture->getPlaneDimensions(i);
        size_t bpp = getBytesPerPixel(picture->format, i);
        copyPlaneWithStride(dst, dst_stride, picture->planes.data[i], picture->planes.linesize[i], dims.first * bpp, dims.second);
    }
    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);

    CMTime pts = CMTimeMake(frame.pts, timescale_);
    
    std::vector<Packet> packets;
    {
        std::lock_guard<std::mutex> lock(callback_context_.mutex);
        callback_context_.packets.clear();
    }
    
    status = VTCompressionSessionEncodeFrame(session_, pixel_buffer, pts, kCMTimeInvalid, nullptr, nullptr, nullptr);
    CFRelease(pixel_buffer);

    if (status != noErr) {
        return Err(OM_CODEC_ENCODE_FAILED);
    }

    {
        std::lock_guard<std::mutex> lock(callback_context_.mutex);
        packets = std::move(callback_context_.packets);
        callback_context_.packets.clear();
    }

    std::vector<uint8_t> extra;
    {
        std::lock_guard<std::mutex> lock(callback_context_.mutex);
        extra = callback_context_.extradata;
    }
    if (extradata_.empty() && !extra.empty()) {
        extradata_ = std::move(extra);
    }

    return Ok(std::move(packets));
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    if (!session_) return OM_COMMON_NOT_INITIALIZED;

    int64_t target_bitrate = 0;
    int64_t max_bitrate = 0;

    switch (rc.getMode()) {
        case RateControlMode::CBR:
            target_bitrate = std::get<CbrParams>(rc.params).bitrate.target_bitrate;
            max_bitrate = std::get<CbrParams>(rc.params).bitrate.max_bitrate.value_or(target_bitrate);
            break;
        case RateControlMode::VBR:
            target_bitrate = std::get<VbrParams>(rc.params).bitrate.target_bitrate;
            max_bitrate = std::get<VbrParams>(rc.params).bitrate.max_bitrate.value_or(target_bitrate * 2);
            break;
        case RateControlMode::ABR:
            target_bitrate = std::get<AbrParams>(rc.params).target_bitrate;
            break;
        default:
            break;
    }

    if (target_bitrate > 0) {
        CFNumberRef br = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &target_bitrate);
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_AverageBitRate, br);
        CFRelease(br);
    }

    if (max_bitrate > 0) {
        int64_t bytes_per_second = max_bitrate / 8;
        CFNumberRef limit_bytes = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &bytes_per_second);
        double one = 1.0;
        CFNumberRef limit_duration = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &one);
        const void* values[] = { limit_bytes, limit_duration };
        CFArrayRef limits = CFArrayCreate(kCFAllocatorDefault, values, 2, &kCFTypeArrayCallBacks);
        VTSessionSetProperty(session_, kVTCompressionPropertyKey_DataRateLimits, limits);
        CFRelease(limits);
        CFRelease(limit_bytes);
        CFRelease(limit_duration);
    }

    return OM_SUCCESS;
  }

private:
  void closeSession() {
    initialized_ = false;
    if (session_) {
      VTCompressionSessionInvalidate(session_);
      CFRelease(session_);
      session_ = nullptr;
    }
    extradata_.clear();
    std::lock_guard<std::mutex> lock(callback_context_.mutex);
    callback_context_.extradata.clear();
    callback_context_.packets.clear();
  }

  OMCodecId codec_id_;
  VTCompressionSessionRef session_ = nullptr;
  EncoderCallbackContext callback_context_;
  std::vector<uint8_t> extradata_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  int32_t timescale_ = kDefaultTimeScale;
  bool initialized_ = false;
};

auto createVideoToolboxDecoder(OMCodecId codec_id) -> std::unique_ptr<Decoder> {
  return std::make_unique<VideoToolboxDecoder>(codec_id);
}

auto createVideoToolboxEncoder(OMCodecId codec_id) -> std::unique_ptr<Encoder> {
  return std::make_unique<VideoToolboxEncoder>(codec_id);
}

} // namespace

const CodecDescriptor CODEC_VIDEOTOOLBOX_H263 = {
    .codec_id = OM_CODEC_H263,
    .type = OM_MEDIA_VIDEO,
    .name = "videotoolbox_h263",
    .long_name = "H.263 (VideoToolbox)",
    .vendor = "Apple",
    .flags = HARDWARE,
    .caps = CodecCaps {.video = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12}}},
    .decoder_factory = [] { return createVideoToolboxDecoder(OM_CODEC_H263); },
};

const CodecDescriptor CODEC_VIDEOTOOLBOX_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "videotoolbox_h264",
    .long_name = "H.264 / AVC (VideoToolbox)",
    .vendor = "Apple",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_H264_BASELINE,
                     OM_PROFILE_H264_CONSTRAINED_BASELINE,
                     OM_PROFILE_H264_MAIN,
                     OM_PROFILE_H264_EXTENDED,
                     OM_PROFILE_H264_HIGH},
        .video = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12}},
    },
    .decoder_factory = [] { return createVideoToolboxDecoder(OM_CODEC_H264); },
    .encoder_factory = [] { return createVideoToolboxEncoder(OM_CODEC_H264); },
};

const CodecDescriptor CODEC_VIDEOTOOLBOX_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "videotoolbox_hevc",
    .long_name = "H.265 / HEVC (VideoToolbox)",
    .vendor = "Apple",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_H265_MAIN,
                     OM_PROFILE_H265_MAIN_10,
                     OM_PROFILE_H265_MAIN_STILL_PICTURE,
                     OM_PROFILE_H265_REXT},
        .video = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12, OM_FORMAT_P010, OM_FORMAT_P012, OM_FORMAT_P016}},
    },
    .decoder_factory = [] { return createVideoToolboxDecoder(OM_CODEC_H265); },
    .encoder_factory = [] { return createVideoToolboxEncoder(OM_CODEC_H265); },
};

const CodecDescriptor CODEC_VIDEOTOOLBOX_MPEG2 = {
    .codec_id = OM_CODEC_MPEG2,
    .type = OM_MEDIA_VIDEO,
    .name = "videotoolbox_mpeg2",
    .long_name = "MPEG-2 Video (VideoToolbox)",
    .vendor = "Apple",
    .flags = HARDWARE,
    .caps = CodecCaps {.video = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12}}},
    .decoder_factory = [] { return createVideoToolboxDecoder(OM_CODEC_MPEG2); },
};

const CodecDescriptor CODEC_VIDEOTOOLBOX_MPEG4 = {
    .codec_id = OM_CODEC_MPEG4,
    .type = OM_MEDIA_VIDEO,
    .name = "videotoolbox_mpeg4",
    .long_name = "MPEG-4 Part 2 Video (VideoToolbox)",
    .vendor = "Apple",
    .flags = HARDWARE,
    .caps = CodecCaps {.video = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12}}},
    .decoder_factory = [] { return createVideoToolboxDecoder(OM_CODEC_MPEG4); },
};

const CodecDescriptor CODEC_VIDEOTOOLBOX_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "videotoolbox_av1",
    .long_name = "AV1 (VideoToolbox)",
    .vendor = "Apple",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_AV1_MAIN, OM_PROFILE_AV1_HIGH, OM_PROFILE_AV1_PROFESSIONAL},
        .video = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12, OM_FORMAT_P010, OM_FORMAT_P012, OM_FORMAT_P016}},
    },
    .decoder_factory = [] { return createVideoToolboxDecoder(OM_CODEC_AV1); },
};

const CodecDescriptor CODEC_VIDEOTOOLBOX_PRORES = {
    .codec_id = OM_CODEC_PRORES,
    .type = OM_MEDIA_VIDEO,
    .name = "videotoolbox_prores",
    .long_name = "Apple ProRes (VideoToolbox)",
    .vendor = "Apple",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_PRORES_PROXY,
                     OM_PROFILE_PRORES_LT,
                     OM_PROFILE_PRORES_STANDARD,
                     OM_PROFILE_PRORES_HQ,
                     OM_PROFILE_PRORES_4444,
                     OM_PROFILE_PRORES_XQ},
        .video = VideoCodecCaps {.pix_fmts = {OM_FORMAT_NV12}},
    },
    .decoder_factory = [] { return createVideoToolboxDecoder(OM_CODEC_PRORES); },
    .encoder_factory = [] { return createVideoToolboxEncoder(OM_CODEC_PRORES); },
};

} // namespace openmedia
