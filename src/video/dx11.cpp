#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d9types.h>
#include <dxva.h>
#include <dxva2api.h>
#include <guiddef.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <openmedia/hw_dx11.h>
#include <wmcodecdsp.h>
#include <algorithm>
#include <codecs.hpp>
#include <cstring>
#include <memory>
#include <openmedia/video.hpp>
#include <vector>

namespace openmedia {

static const GUID DXVA2_ModeH264_E = {0x1b81be68, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
static const GUID DXVA2_ModeH264_F = {0x1b81be69, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
static const GUID DXVA2_ModeHEVC_VLD_Main = {0x5b11d51b, 0x2f4c, 0x4452, {0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0}};
static const GUID DXVA2_ModeHEVC_VLD_Main10 = {0x10000000, 0x0001, 0x0001, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
static const GUID DXVA2_ModeVP9_VLD_Profile0 = {0x463707f8, 0xa1d0, 0x4585, {0x87, 0x6d, 0x83, 0xaa, 0x6d, 0x60, 0xb8, 0x9e}};
static const GUID DXVA2_ModeVP9_VLD_10bit_Profile2 = {0xa4c749ef, 0x6ecf, 0x48aa, {0x84, 0x48, 0x50, 0xa7, 0xa1, 0x16, 0x5f, 0xf7}};

struct DX11DecoderContextDeleter {
  void operator()(OMDX11Context* ctx) const {
    if (ctx) {
      HWD3D11Context_delete(ctx);
    }
  }
};

struct DX11PictureDeleter {
  void operator()(OMDX11Picture* pic) const {
    if (pic) {
      HWD3D11Picture_delete(pic);
    }
  }
};

class DX11HardwarePicture : public HardwarePicture {
public:
  std::shared_ptr<OMDX11Picture> picture;

  DX11HardwarePicture(std::shared_ptr<OMDX11Picture> pic)
      : HardwarePicture(HWDeviceType::DX11), picture(std::move(pic)) {}

  ~DX11HardwarePicture() override = default;
};

template<typename T>
class ComPtr {
public:
  ComPtr() = default;
  explicit ComPtr(T* ptr)
      : ptr_(ptr) {}
  ~ComPtr() { reset(); }

  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;

  ComPtr(ComPtr&& other) noexcept
      : ptr_(other.ptr_) { other.ptr_ = nullptr; }
  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      reset();
      ptr_ = other.ptr_;
      other.ptr_ = nullptr;
    }
    return *this;
  }

  void reset() {
    if (ptr_) {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }

  T* get() const { return ptr_; }
  T** getAddressOf() { return &ptr_; }
  T* operator->() const { return ptr_; }
  operator T*() const { return ptr_; }

private:
  T* ptr_ = nullptr;
};

class DX11Decoder final : public Decoder {
  std::unique_ptr<OMDX11Context, DX11DecoderContextDeleter> hw_context_;
  LoggerRef logger_ = {};
  bool initialized_ = false;
  VideoFormat output_format_ = {};
  OMMediaType media_type_ = OM_MEDIA_NONE;

  ComPtr<IMFTransform> decoder_transform_;
  GUID decoder_guid_ = GUID_NULL;
  UINT32 sample_count_ = 0;

  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::vector<uint8_t> extradata_;

  // Frame reordering for B-frames
  std::vector<std::pair<LONGLONG, std::shared_ptr<OMDX11Picture>>> pending_frames_;

public:
  DX11Decoder() {}

  ~DX11Decoder() override {
    flush();
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_H264 &&
        options.format.codec_id != OM_CODEC_H265 &&
        options.format.codec_id != OM_CODEC_VP9) {
      logger_ = options.logger ? options.logger : Logger::refDefault();
      if (logger_) {
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_WARNING,
                     "DX11 decoder only supports H264, H265, and VP9");
      }
      return OM_CODEC_NOT_SUPPORTED;
    }

    codec_id_ = options.format.codec_id;
    width_ = options.format.video.width;
    height_ = options.format.video.height;

    if (width_ == 0 || height_ == 0) {
      return OM_CODEC_INVALID_PARAMS;
    }

    if (!options.extradata.empty()) {
      extradata_.assign(options.extradata.begin(), options.extradata.end());
    }

    logger_ = options.logger ? options.logger : Logger::refDefault();

    if (!selectDecoderGuid()) {
      return OM_CODEC_NOT_SUPPORTED;
    }

    OMDX11Init init = {};
    init.device = nullptr;
    init.device_context = nullptr;
    init.adapter_index = -1; // Use default adapter

    hw_context_.reset(HWD3D11Context_create(init));
    if (!hw_context_) {
      if (logger_) {
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR,
                     "Failed to create DX11 hardware context");
      }
      return OM_CODEC_HWACCEL_FAILED;
    }

    // Create Media Foundation decoder transform
    HRESULT hr = createDecoderTransform();
    if (FAILED(hr)) {
      if (logger_) {
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR,
                     "Failed to create decoder transform");
      }
      return OM_CODEC_HWACCEL_FAILED;
    }

    output_format_.width = width_;
    output_format_.height = height_;
    output_format_.format = OM_FORMAT_NV12; // DXVA2 typically outputs NV12
    media_type_ = OM_MEDIA_VIDEO;
    initialized_ = true;

    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;

    DecodingInfo info = {};
    info.media_type = media_type_;
    info.video_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    if (!initialized_) {
      return Err(OM_COMMON_NOT_INITIALIZED);
    }

    if (packet.bytes.empty()) {
      // Flush/drain request
      return drainFrames(frames);
    }

    HRESULT hr = processInput(packet);
    if (FAILED(hr)) {
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        return Ok(std::move(frames));
      }
      return Err(OM_CODEC_DECODE_FAILED);
    }

    return processOutput(frames);
  }

  void flush() override {
    if (decoder_transform_) {
      decoder_transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    pending_frames_.clear();
  }

private:
  bool selectDecoderGuid() {
    switch (codec_id_) {
      case OM_CODEC_H264:
        // Try H264 VLD (Variable Length Decoding) modes
        decoder_guid_ = DXVA2_ModeH264_F; // H264 VLD mode F (most common)
        return true;
      case OM_CODEC_H265:
        decoder_guid_ = DXVA2_ModeHEVC_VLD_Main;
        return true;
      case OM_CODEC_VP9:
        decoder_guid_ = DXVA2_ModeVP9_VLD_Profile0;
        return true;
      default:
        return false;
    }
  }

  HRESULT createDecoderTransform() {
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr) && hr != MF_S_MULTIPLE_BEGIN) {
      return hr;
    }

    //hr = CoCreateInstance(CLSID_VideoProcessorMFT, nullptr, CLSCTX_INPROC_SERVER,
    //                      IID_PPV_ARGS(decoder_transform_.getAddressOf()));

    if (FAILED(hr)) {
      switch (codec_id_) {
        case OM_CODEC_H264:
          //hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
          //                      IID_PPV_ARGS(decoder_transform_.getAddressOf()));
          break;
      }
    }

    if (FAILED(hr)) {
      return hr;
    }

    ComPtr<IMFMediaType> input_type;
    hr = MFCreateMediaType(input_type.getAddressOf());
    if (FAILED(hr)) return hr;

    hr = input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return hr;

    GUID subtype = GUID_NULL;
    switch (codec_id_) {
      case OM_CODEC_H264:
        subtype = MFVideoFormat_H264;
        break;
      case OM_CODEC_H265:
        subtype = MFVideoFormat_HEVC;
        break;
      case OM_CODEC_VP9:
        subtype = MFVideoFormat_VP90;
        break;
      default:
        return E_FAIL;
    }

    hr = input_type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (FAILED(hr)) return hr;

    hr = MFSetAttributeSize(input_type.get(), MF_MT_FRAME_SIZE, width_, height_);
    if (FAILED(hr)) return hr;

    hr = input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) return hr;

    hr = decoder_transform_->SetInputType(0, input_type.get(), 0);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaType> output_type;
    hr = MFCreateMediaType(output_type.getAddressOf());
    if (FAILED(hr)) return hr;

    hr = output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return hr;

    hr = output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(hr)) return hr;

    hr = MFSetAttributeSize(output_type.get(), MF_MT_FRAME_SIZE, width_, height_);
    if (FAILED(hr)) return hr;

    hr = output_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(hr)) return hr;

    hr = decoder_transform_->SetOutputType(0, output_type.get(), 0);
    if (FAILED(hr)) return hr;

    hr = decoder_transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) return hr;

    hr = decoder_transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) return hr;

    return S_OK;
  }

  HRESULT processInput(const Packet& packet) {
    if (!decoder_transform_) return E_FAIL;

    ComPtr<IMFMediaBuffer> input_buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(packet.bytes.size()),
                                      input_buffer.getAddressOf());
    if (FAILED(hr)) return hr;

    BYTE* buffer_data = nullptr;
    hr = input_buffer->Lock(&buffer_data, nullptr, nullptr);
    if (FAILED(hr)) return hr;

    memcpy(buffer_data, packet.bytes.data(), packet.bytes.size());
    hr = input_buffer->Unlock();
    if (FAILED(hr)) return hr;

    hr = input_buffer->SetCurrentLength(static_cast<DWORD>(packet.bytes.size()));
    if (FAILED(hr)) return hr;

    ComPtr<IMFSample> input_sample;
    hr = MFCreateSample(input_sample.getAddressOf());
    if (FAILED(hr)) return hr;

    hr = input_sample->AddBuffer(input_buffer.get());
    if (FAILED(hr)) return hr;

    // Set timestamp
    LONGLONG pts = packet.pts;
    hr = input_sample->SetSampleTime(pts);
    if (FAILED(hr)) return hr;

    // Set key frame flag
    if (packet.is_keyframe) {
      hr = input_sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    }

    // Send to decoder
    hr = decoder_transform_->ProcessInput(0, input_sample.get(), 0);
    return hr;
  }

  auto processOutput(std::vector<Frame>& frames) -> Result<std::vector<Frame>, OMError> {
    if (!decoder_transform_) return Err(OM_COMMON_NOT_INITIALIZED);

    MFT_OUTPUT_DATA_BUFFER output_data = {};
    DWORD status = 0;

    ComPtr<IMFSample> output_sample;
    HRESULT hr = MFCreateSample(output_sample.getAddressOf());
    if (FAILED(hr)) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    ComPtr<IMFMediaBuffer> output_buffer;
    hr = MFCreateMemoryBuffer(width_ * height_ * 3 / 2, output_buffer.getAddressOf());
    if (FAILED(hr)) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    hr = output_sample->AddBuffer(output_buffer.get());
    if (FAILED(hr)) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    output_data.dwStreamID = 0;
    output_data.pSample = output_sample.get();
    output_data.dwStatus = 0;

    hr = decoder_transform_->ProcessOutput(0, 1, &output_data, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      return Ok(std::move(frames));
    }

    if (FAILED(hr)) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    ComPtr<IMFMediaBuffer> result_buffer;
    hr = output_sample->ConvertToContiguousBuffer(result_buffer.getAddressOf());
    if (FAILED(hr)) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    LONGLONG pts = 0;
    output_sample->GetSampleTime(&pts);

    auto hw_picture = std::make_shared<DX11HardwarePicture>(nullptr);

    Frame frame = {};
    frame.pts = pts;
    frame.dts = pts;
    frames.push_back(std::move(frame));

    return Ok(std::move(frames));
  }

  auto drainFrames(std::vector<Frame>& frames) -> Result<std::vector<Frame>, OMError> {
    if (!decoder_transform_) return Ok(std::move(frames));

    HRESULT hr = decoder_transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(hr)) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    while (true) {
      auto result = processOutput(frames);
      if (!result.isOk() || result.unwrap().empty()) {
        break;
      }
    }

    return Ok(std::move(frames));
  }
};

const CodecDescriptor CODEC_DX11_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "dx11_h264",
    .long_name = "DirectX11 H.264 Decoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_H264_BASELINE, OM_PROFILE_H264_MAIN, OM_PROFILE_H264_HIGH},
    },
    .decoder_factory = [] { return std::make_unique<DX11Decoder>(); },
};

} // namespace openmedia
