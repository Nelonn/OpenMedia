#include <d3d12.h>
#include <d3d12video.h>
#include <dxva.h>
#include <mfapi.h>
#include <mferror.h>
#include <openmedia/hw_dx12.h>
#include <algorithm>
#include <codecs.hpp>
#include <cstring>
#include <memory>
#include <openmedia/video.hpp>
#include <vector>
#include <wrl/client.h>

namespace openmedia {

/*using Microsoft::WRL::ComPtr;

struct DX12DecoderContextDeleter {
  void operator()(OMDX12Context* ctx) const {
    if (ctx) {
      HWD3D12Context_delete(ctx);
    }
  }
};

struct DX12PictureDeleter {
  void operator()(OMDX12Picture* pic) const {
    if (pic) {
      HWD3D12Picture_delete(pic);
    }
  }
};

class DX12HardwarePicture : public HardwarePicture {
public:
  std::shared_ptr<OMDX12Picture> picture;

  DX12HardwarePicture(std::shared_ptr<OMDX12Picture> pic)
      : HardwarePicture(HWDeviceType::DX12), picture(std::move(pic)) {}

  ~DX12HardwarePicture() override = default;
};

static auto getCodecD3D12(OMCodecId codec_id, OMProfile profile) -> GUID {
  switch (codec_id) {
    case OM_CODEC_H264:
      return D3D12_VIDEO_DECODE_PROFILE_H264;
    case OM_CODEC_H265:
      switch (profile) {
      case OM_PROFILE_H265_MAIN:
          return D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN;
      case OM_PROFILE_H265_MAIN_10:
          return D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10;
      case OM_PROFILE_H265_MAIN_12:
          return D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN12;
      case OM_PROFILE_H265_MAIN_4_2_2_10:
          return D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10_422;
      case OM_PROFILE_H265_MAIN_4_2_2_12:
          return D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN12_422;
      case OM_PROFILE_H265_MAIN_4_4_4_10:
          return D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN10_444;
      case OM_PROFILE_H265_MAIN_4_4_4_12:
          return D3D12_VIDEO_DECODE_PROFILE_HEVC_MAIN12_444;
      default:
          return {};
      }
    case OM_CODEC_VP9:
      switch (profile) {
      default:
      case OM_PROFILE_VP9_0:
          return D3D12_VIDEO_DECODE_PROFILE_VP9;
      case OM_PROFILE_VP9_1:
      case OM_PROFILE_VP9_2:
      case OM_PROFILE_VP9_3:
          return D3D12_VIDEO_DECODE_PROFILE_VP9_10BIT_PROFILE2;
      }
    case OM_CODEC_AV1:
      switch (profile) {
      case OM_PROFILE_AV1_MAIN:
          return D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0;
      case OM_PROFILE_AV1_HIGH:
          return D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE1;
      case OM_PROFILE_AV1_PROFESSIONAL:
          return D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE2;
      default: return {};
      }
    default:
      return {};
  }
}

class DX12Decoder final : public Decoder {
  std::unique_ptr<OMDX12Context, DX12DecoderContextDeleter> hw_context_;
  LoggerRef logger_ = {};
  bool initialized_ = false;
  VideoFormat output_format_ = {};
  OMMediaType media_type_ = OM_MEDIA_NONE;

  ComPtr<ID3D12VideoDevice> video_device_;
  ComPtr<ID3D12VideoDecodeCommandList> decode_command_list_;
  ComPtr<ID3D12CommandAllocator> command_allocator_;
  ComPtr<ID3D12Fence> fence_;
  HANDLE fence_event_ = nullptr;
  UINT64 fence_value_ = 0;

  D3D12_VIDEO_DECODE_CONFIGURATION decode_config_ = {};
  ComPtr<ID3D12VideoDecoder> video_decoder_;
  ComPtr<ID3D12VideoDecoderHeap> decoder_heap_;

  OMCodecId codec_id_ = OM_CODEC_NONE;
  OMProfile profile_ = OM_PROFILE_NONE;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::vector<uint8_t> extradata_;

  std::vector<std::shared_ptr<OMDX12Picture>> reference_frames_;

public:
  DX12Decoder() {}

  ~DX12Decoder() override {
    flush();

    if (fence_event_) {
      CloseHandle(fence_event_);
      fence_event_ = nullptr;
    }
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_H264 &&
        options.format.codec_id != OM_CODEC_H265 &&
        options.format.codec_id != OM_CODEC_VP9 &&
        options.format.codec_id != OM_CODEC_AV1) {
      logger_ = options.logger ? options.logger : Logger::refDefault();
      if (logger_) {
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_WARNING,
                     "DX12 decoder only supports H264, H265, VP9, and AV1");
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

    OMDX12Init init = {};
    init.device = nullptr;
    init.command_queue = nullptr;
    init.adapter_index = -1;

    hw_context_.reset(HWD3D12Context_create(init));
    if (!hw_context_) {
      if (logger_) {
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR,
                     "Failed to create DX12 hardware context");
      }
      return OM_CODEC_HWACCEL_FAILED;
    }

    video_device_ = HWD3D12Context_getVideoDevice(hw_context_.get());
    decode_command_list_ = HWD3D12Context_getDecodeCommandList(hw_context_.get());

    if (!video_device_ || !decode_command_list_) {
      if (logger_) {
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR,
                     "Failed to get video interfaces");
      }
      return OM_CODEC_HWACCEL_FAILED;
    }

    HRESULT hr = hw_context_->device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, IID_PPV_ARGS(&command_allocator_));
    if (FAILED(hr)) {
      if (logger_) {
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR,
                     "Failed to create command allocator");
      }
      return OM_CODEC_HWACCEL_FAILED;
    }

    hr = hw_context_->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr)) {
      return OM_CODEC_HWACCEL_FAILED;
    }

    fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
      return OM_CODEC_HWACCEL_FAILED;
    }

    hr = createVideoDecoder();
    if (FAILED(hr)) {
      if (logger_) {
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR,
                     "Failed to create video decoder");
      }
      return OM_CODEC_HWACCEL_FAILED;
    }

    hr = createDecoderHeap();
    if (FAILED(hr)) {
      if (logger_) {
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR,
                     "Failed to create decoder heap");
      }
      return OM_CODEC_HWACCEL_FAILED;
    }

    output_format_.width = width_;
    output_format_.height = height_;
    output_format_.format = OM_FORMAT_NV12;
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
      return drainFrames(frames);
    }

    HRESULT hr = decodeFrame(packet, frames);
    if (FAILED(hr)) {
      if (hr == E_PENDING) {
        return Ok(std::move(frames));
      }
      return Err(OM_CODEC_DECODE_FAILED);
    }

    return Ok(std::move(frames));
  }

  void flush() override {
    reference_frames_.clear();

    if (decode_command_list_) {
      decode_command_list_->Reset(command_allocator_.Get());
    }
  }

private:

  HRESULT createVideoDecoder() {
    if (!video_device_) return E_FAIL;

    decode_config_.DecodeProfile = getCodecD3D12(codec_id_, profile_);
		decode_config_.BitstreamEncryption = D3D12_BITSTREAM_ENCRYPTION_TYPE_NONE;
		decode_config_.InterlaceType = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;

    D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT support = {};
    support.NodeIndex = 0;
    support.Configuration = decode_config_;
    support.Width = width_;
    support.Height = height_;

    HRESULT hr = video_device_->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_DECODE_SUPPORT,
        &support,
        sizeof(support));

    if (FAILED(hr) || !support.Supported) {
      if (logger_) {
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR,
                     "Decoder configuration not supported by hardware");
      }
      return E_FAIL;
    }

    if (FAILED(hr)) {
      return hr;
    }

    // Create the decoder
    hr = video_device_->CreateVideoDecoder(
        &decode_config_,
        IID_PPV_ARGS(&video_decoder_));

    return hr;
  }

  HRESULT createDecoderHeap() {
    if (!video_device_ || !video_decoder_) return E_FAIL;

    D3D12_VIDEO_DECODE_HEAP_FLAGS flags = D3D12_VIDEO_DECODE_HEAP_FLAG_NONE;

    D3D12_VIDEO_DECODE_CONVERSION_SUPPORT support = {};
    support.NodeIndex = 0;
    support.DecodeFormat = {DXGI_FORMAT_NV12, DXGI_COLOR_SPACE_TYPE_RGB_FULL_G22_NONE_P709};
    support.InputSampleFormat = support.DecodeFormat;
    support.Flags = D3D12_VIDEO_PROCESS_ALPHA_BLENDING;
    support.DecodeWidth = width_;
    support.DecodeHeight = height_;

    D3D12_FEATURE_DATA_VIDEO_DECODE_HEAP_SIZE heap_size = {};
    heap_size.Configuration = decode_config_;
    heap_size.Width = width_;
    heap_size.Height = height_;

    HRESULT hr = video_device_->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_DECODE_HEAP_SIZE,
        &heap_size,
        sizeof(heap_size));

    if (FAILED(hr)) {
      return hr;
    }

    D3D12_VIDEO_DECODE_HEAP_DESC heap_desc = {};
    heap_desc.DecodeConfiguration = decode_config_;
    heap_desc.Width = width_;
    heap_desc.Height = height_;
    heap_desc.Flags = flags;

    hr = video_device_->CreateVideoDecoderHeap(
        &heap_desc,
        IID_PPV_ARGS(decoder_heap_.getAddressOf()));

    return hr;
  }

  HRESULT decodeFrame(const Packet& packet, std::vector<Frame>& frames) {
    if (!video_decoder_ || !decoder_heap_ || !decode_command_list_) {
      return E_FAIL;
    }

    HRESULT hr = decode_command_list_->Reset(command_allocator_.Get());
    if (FAILED(hr)) return hr;

    auto picture = std::make_shared<OMDX12Picture>();

    D3D12_RESOURCE_DESC texture_desc = {};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = width_;
    texture_desc.Height = height_;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_NV12;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.SampleDesc.Quality = 0;
    texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heap_props = {};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    hr = hw_context_->device->CreateCommittedResource(
        &heap_props,
        D3D12_HEAP_FLAG_NONE,
        &texture_desc,
        D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE,
        nullptr,
        IID_PPV_ARGS(&picture->texture));

    if (FAILED(hr)) {
      return hr;
    }

    picture->reference_frames.NumTextures = reference_frame_count_;
    picture->reference_frames.pTexture2D = static_cast<ID3D12Resource**>(
        malloc(sizeof(ID3D12Resource*) * reference_frame_count_));

    for (UINT i = 0; i < reference_frame_count_; i++) {
      if (i < reference_frames_.size() && reference_frames_[i]) {
        picture->reference_frames.pTexture2D[i] = reference_frames_[i]->texture;
        if (picture->reference_frames.pTexture2D[i]) {
          picture->reference_frames.pTexture2D[i]->AddRef();
        }
      } else {
        picture->reference_frames.pTexture2D[i] = nullptr;
      }
    }

    D3D12_VIDEO_DECODE_FRAME_ARGUMENT frame_args = {};
    frame_args.ArgumentType = D3D12_VIDEO_DECODE_FRAME_ARGUMENT_TYPE_COMPRESSEDBUFFER;
    frame_args.pBuffer = nullptr;
    frame_args.Size = packet.bytes.size();
    frame_args.DataOffset = 0;

    decode_command_list_->BeginQuery(video_decoder_.get(), decoder_heap_.get(), 0);

    decode_command_list_->DecodeFrame(
        video_decoder_.get(),
        decoder_heap_.get(),
        picture->texture,
        0,
        nullptr);

    decode_command_list_->EndQuery(video_decoder_.get(), 0);

    hr = decode_command_list_->Close();
    if (FAILED(hr)) return hr;

    ID3D12CommandList* command_lists[] = {decode_command_list_.get()};
    hw_context_->command_queue->ExecuteCommandLists(1, command_lists);

    hr = hw_context_->command_queue->Signal(fence_.get(), fence_value_ + 1);
    if (FAILED(hr)) return hr;
    fence_value_++;

    // Wait for completion
    if (fence_->GetCompletedValue() < fence_value_) {
      hr = fence_->SetEventOnCompletion(fence_value_, fence_event_);
      if (SUCCEEDED(hr)) {
        WaitForSingleObject(fence_event_, 1000);
      }
    }

    reference_frames_.push_back(picture);
    if (reference_frames_.size() > reference_frame_count_) {
      reference_frames_.erase(reference_frames_.begin());
    }

    Frame frame = {};
    frame.pts = packet.pts;
    frame.dts = packet.dts;

    frames.push_back(std::move(frame));

    return S_OK;
  }

  auto drainFrames(std::vector<Frame>& frames) -> Result<std::vector<Frame>, OMError> {
    return Ok(std::move(frames));
  }
};

const CodecDescriptor CODEC_DX12_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "dx12_h264",
    .long_name = "DirectX12 H.264 Decoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_H264_BASELINE, OM_PROFILE_H264_MAIN, OM_PROFILE_H264_HIGH},
    },
    .decoder_factory = [] { return std::make_unique<DX12Decoder>(); },
};

const CodecDescriptor CODEC_DX12_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "dx12_h265",
    .long_name = "DirectX12 H.265 Decoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_H265_MAIN, OM_PROFILE_H265_MAIN_10},
    },
    .decoder_factory = [] { return std::make_unique<DX12Decoder>(); },
};

const CodecDescriptor CODEC_DX12_VP9 = {
    .codec_id = OM_CODEC_VP9,
    .type = OM_MEDIA_VIDEO,
    .name = "dx12_vp9",
    .long_name = "DirectX12 VP9 Decoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_VP9_0, OM_PROFILE_VP9_1, OM_PROFILE_VP9_2, OM_PROFILE_VP9_3},
    },
    .decoder_factory = [] { return std::make_unique<DX12Decoder>(); },
};

const CodecDescriptor CODEC_DX12_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "dx12_av1",
    .long_name = "DirectX12 AV1 Decoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_AV1_MAIN, OM_PROFILE_AV1_HIGH},
    },
    .decoder_factory = [] { return std::make_unique<DX12Decoder>(); },
};*/

} // namespace openmedia
