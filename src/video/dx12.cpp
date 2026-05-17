#include <openmedia/hw_dx12.h>
#include <openmedia/video.hpp>

#include <algorithm>
#include <codecs.hpp>
#include <cstdint>
#include <cstring>
#include <d3d12.h>
#include <d3d12video.h>
#include <dxgi1_6.h>
#include <dxva.h>
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "dx_h264.hpp"

namespace openmedia {

using Microsoft::WRL::ComPtr;

class DX12Decoder final : public Decoder {
  static constexpr uint64_t BITSTREAM_SIZE = 8ull * 1024ull * 1024ull;

  bool initialized_ = false;
  VideoFormat output_format_ = {};

  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12VideoDevice> video_device_;
  ComPtr<ID3D12CommandQueue> video_queue_;
  ComPtr<ID3D12CommandQueue> copy_queue_;
  ComPtr<ID3D12CommandAllocator> video_allocator_;
  ComPtr<ID3D12VideoDecodeCommandList> video_cmd_;
  ComPtr<ID3D12CommandAllocator> copy_allocator_;
  ComPtr<ID3D12GraphicsCommandList> copy_cmd_;
  ComPtr<ID3D12Fence> video_fence_;
  ComPtr<ID3D12Fence> copy_fence_;
  HANDLE video_event_ = nullptr;
  HANDLE copy_event_ = nullptr;
  uint64_t video_fence_value_ = 0;
  uint64_t copy_fence_value_ = 0;

  ComPtr<ID3D12VideoDecoder> decoder_;
  ComPtr<ID3D12VideoDecoderHeap> decoder_heap_;
  ComPtr<ID3D12Resource> bitstream_buffer_;
  uint8_t* bitstream_ptr_ = nullptr;
  ComPtr<ID3D12Resource> dpb_texture_;
  ComPtr<ID3D12Resource> output_texture_;
  bool reference_only_ = false;

  dx_h264::State h264_;
  std::vector<dx_h264::DpbEntry> dpb_;
  D3D12_RESOURCE_STATES dpb_states_[17] = {};
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t padded_width_ = 0;
  uint32_t padded_height_ = 0;
  uint32_t dpb_slot_count_ = 17;
  uint32_t next_slot_ = 0;
  uint32_t next_ref_ = 0;
  uint32_t feedback_ = 1;
  std::vector<uint8_t> reference_usage_;

public:
  ~DX12Decoder() override { release(); }

  auto configure(const DecoderOptions& options) -> OMError override {
    release();
    if (options.format.codec_id != OM_CODEC_H264) return OM_CODEC_NOT_SUPPORTED;
    width_ = options.format.video.width;
    height_ = options.format.video.height;
    if (width_ == 0 || height_ == 0) return OM_CODEC_INVALID_PARAMS;

    h264_.parseExtradata(options.extradata);
    padded_width_ = dx_h264::alignUp(width_, 16u);
    padded_height_ = dx_h264::alignUp(height_, 16u);
    if (h264_.has_sps) {
      for (uint32_t i = 0; i < 32; ++i) {
        if (!h264_.sps_valid[i]) continue;
        padded_width_ = static_cast<uint32_t>((h264_.sps[i].pic_width_in_mbs_minus1 + 1) * 16);
        padded_height_ = static_cast<uint32_t>((h264_.sps[i].pic_height_in_map_units_minus1 + 1) * 16);
        dpb_slot_count_ = std::clamp<uint32_t>(h264_.sps[i].num_ref_frames + 1, 2, 17);
        break;
      }
    }

    if (options.hw_device && options.hw_device->type == HWDeviceType::DX12 && options.hw_device->context) {
      auto* ctx = static_cast<OMDX12Context*>(options.hw_device->context);
      device_ = HWD3D12Context_getDevice(ctx);
      video_device_ = HWD3D12Context_getVideoDevice(ctx);
    } else {
      if (FAILED(createDevice())) return OM_CODEC_HWACCEL_FAILED;
    }
    if (!device_ || !video_device_) return OM_CODEC_HWACCEL_FAILED;

    if (FAILED(createQueuesAndCommands())) return OM_CODEC_HWACCEL_FAILED;
    if (FAILED(createDecoder())) return OM_CODEC_HWACCEL_FAILED;
    if (FAILED(createResources())) return OM_CODEC_HWACCEL_FAILED;

    output_format_ = {OM_FORMAT_NV12, width_, height_};
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;
    DecodingInfo info = {};
    info.media_type = OM_MEDIA_VIDEO;
    info.video_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);
    if (packet.bytes.empty()) return Ok(std::vector<Frame>{});

    auto parsed = h264_.parseFrame(packet.bytes);
    if (parsed.slice_offsets.empty()) return Ok(std::vector<Frame>{});
    if (parsed.bitstream.size() > BITSTREAM_SIZE) return Err(OM_CODEC_DECODE_FAILED);
    if (parsed.slice.pic_parameter_set_id < 0 || parsed.slice.pic_parameter_set_id >= 256 || !h264_.pps_valid[parsed.slice.pic_parameter_set_id]) return Err(OM_CODEC_DECODE_FAILED);
    const auto& pps = h264_.pps[parsed.slice.pic_parameter_set_id];
    if (pps.seq_parameter_set_id < 0 || pps.seq_parameter_set_id >= 32 || !h264_.sps_valid[pps.seq_parameter_set_id]) return Err(OM_CODEC_DECODE_FAILED);
    const auto& sps = h264_.sps[pps.seq_parameter_set_id];
    if (sps.bit_depth_luma_minus8 != 0 || sps.bit_depth_chroma_minus8 != 0 || pps.num_slice_groups_minus1 != 0) return Err(OM_CODEC_NOT_SUPPORTED);

    if (parsed.is_intra) {
      for (auto& entry : dpb_) entry.is_reference = false;
      reference_usage_.clear();
      next_ref_ = 0;
      next_slot_ = 0;
      h264_.resetPoc();
      parsed.poc = h264_.computePoc(parsed.slice);
    }

    std::memcpy(bitstream_ptr_, parsed.bitstream.data(), parsed.bitstream.size());
    const uint32_t current_slot = next_slot_;

    HRESULT hr = recordDecode(parsed, sps, pps, current_slot);
    if (FAILED(hr)) return Err(OM_CODEC_DECODE_FAILED);

    auto picture = download(current_slot);
    if (!picture.has_value()) return Err(OM_CODEC_DECODE_FAILED);

    dpb_[current_slot].poc = parsed.poc;
    dpb_[current_slot].frame_num = static_cast<uint32_t>(parsed.slice.frame_num);
    dpb_[current_slot].is_reference = parsed.is_reference;
    if (parsed.is_reference && dpb_slot_count_ > 1) {
      if (next_ref_ >= reference_usage_.size()) reference_usage_.resize(next_ref_ + 1);
      reference_usage_[next_ref_] = static_cast<uint8_t>(current_slot);
      next_ref_ = (next_ref_ + 1) % (dpb_slot_count_ - 1);
      next_slot_ = (next_slot_ + 1) % dpb_slot_count_;
    }

    Frame frame = {};
    frame.pts = packet.pts;
    frame.dts = packet.dts;
    frame.data = std::move(*picture);
    return Ok(std::vector<Frame>{std::move(frame)});
  }

  void flush() override {
    for (auto& entry : dpb_) entry = {};
    reference_usage_.clear();
    next_slot_ = 0;
    next_ref_ = 0;
    h264_.resetPoc();
  }

private:
  auto createDevice() -> HRESULT {
    ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    for (UINT i = 0;; ++i) {
      ComPtr<IDXGIAdapter1> adapter;
      if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc = {};
      adapter->GetDesc1(&desc);
      if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
      hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
      if (SUCCEEDED(hr) && SUCCEEDED(device_.As(&video_device_))) return S_OK;
      device_.Reset();
      video_device_.Reset();
    }
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
    if (SUCCEEDED(hr)) hr = device_.As(&video_device_);
    return hr;
  }

  auto createQueuesAndCommands() -> HRESULT {
    D3D12_COMMAND_QUEUE_DESC q = {};
    q.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE;
    HRESULT hr = device_->CreateCommandQueue(&q, IID_PPV_ARGS(&video_queue_));
    if (FAILED(hr)) return hr;
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device_->CreateCommandQueue(&q, IID_PPV_ARGS(&copy_queue_));
    if (FAILED(hr)) return hr;
    hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, IID_PPV_ARGS(&video_allocator_));
    if (FAILED(hr)) return hr;
    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE, video_allocator_.Get(), nullptr, IID_PPV_ARGS(&video_cmd_));
    if (FAILED(hr)) return hr;
    video_cmd_->Close();
    hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copy_allocator_));
    if (FAILED(hr)) return hr;
    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy_allocator_.Get(), nullptr, IID_PPV_ARGS(&copy_cmd_));
    if (FAILED(hr)) return hr;
    copy_cmd_->Close();
    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&video_fence_));
    if (FAILED(hr)) return hr;
    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence_));
    if (FAILED(hr)) return hr;
    video_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    copy_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    return (video_event_ && copy_event_) ? S_OK : E_FAIL;
  }

  auto createDecoder() -> HRESULT {
    D3D12_VIDEO_DECODE_CONFIGURATION config = {};
    config.DecodeProfile = D3D12_VIDEO_DECODE_PROFILE_H264;
    config.InterlaceType = D3D12_VIDEO_FRAME_CODED_INTERLACE_TYPE_NONE;

    D3D12_FEATURE_DATA_VIDEO_DECODE_SUPPORT support = {};
    support.Configuration = config;
    support.DecodeFormat = DXGI_FORMAT_NV12;
    support.Width = padded_width_;
    support.Height = padded_height_;
    support.FrameRate = {0, 1};
    HRESULT hr = video_device_->CheckFeatureSupport(D3D12_FEATURE_VIDEO_DECODE_SUPPORT, &support, sizeof(support));
    if (FAILED(hr) || support.DecodeTier < D3D12_VIDEO_DECODE_TIER_1) return FAILED(hr) ? hr : E_FAIL;
    reference_only_ = (support.ConfigurationFlags & D3D12_VIDEO_DECODE_CONFIGURATION_FLAG_REFERENCE_ONLY_ALLOCATIONS_REQUIRED) != 0;

    D3D12_VIDEO_DECODER_HEAP_DESC heap_desc = {};
    heap_desc.Configuration = config;
    heap_desc.DecodeWidth = padded_width_;
    heap_desc.DecodeHeight = padded_height_;
    heap_desc.Format = DXGI_FORMAT_NV12;
    heap_desc.FrameRate = {0, 1};
    heap_desc.MaxDecodePictureBufferCount = dpb_slot_count_;
    if (support.ConfigurationFlags & D3D12_VIDEO_DECODE_CONFIGURATION_FLAG_HEIGHT_ALIGNMENT_MULTIPLE_32_REQUIRED) {
      heap_desc.DecodeWidth = dx_h264::alignUp(padded_width_, 32u);
      heap_desc.DecodeHeight = dx_h264::alignUp(padded_height_, 32u);
    }
    hr = video_device_->CreateVideoDecoderHeap(&heap_desc, IID_PPV_ARGS(&decoder_heap_));
    if (FAILED(hr)) return hr;

    D3D12_VIDEO_DECODER_DESC decoder_desc = {};
    decoder_desc.Configuration = config;
    return video_device_->CreateVideoDecoder(&decoder_desc, IID_PPV_ARGS(&decoder_));
  }

  auto createResources() -> HRESULT {
    D3D12_HEAP_PROPERTIES upload_heap = {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer_desc = {};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer_desc.Width = BITSTREAM_SIZE;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    HRESULT hr = device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&bitstream_buffer_));
    if (FAILED(hr)) return hr;
    hr = bitstream_buffer_->Map(0, nullptr, reinterpret_cast<void**>(&bitstream_ptr_));
    if (FAILED(hr)) return hr;

    D3D12_RESOURCE_DESC tex = {};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex.Format = DXGI_FORMAT_NV12;
    tex.Width = padded_width_;
    tex.Height = padded_height_;
    tex.DepthOrArraySize = static_cast<UINT16>(dpb_slot_count_);
    tex.MipLevels = 1;
    tex.SampleDesc.Count = 1;
    tex.Flags = reference_only_ ? (D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)
                                : D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    D3D12_HEAP_PROPERTIES default_heap = {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    hr = device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&dpb_texture_));
    if (FAILED(hr)) return hr;

    if (reference_only_) {
      tex.DepthOrArraySize = 1;
      tex.Flags = D3D12_RESOURCE_FLAG_NONE;
      hr = device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output_texture_));
      if (FAILED(hr)) return hr;
    }

    dpb_.resize(dpb_slot_count_);
    std::fill(std::begin(dpb_states_), std::end(dpb_states_), D3D12_RESOURCE_STATE_COMMON);
    return S_OK;
  }

  auto recordDecode(const dx_h264::ParsedFrame& parsed, const h264::SPS& sps, const h264::PPS& pps, uint32_t current_slot) -> HRESULT {
    HRESULT hr = video_allocator_->Reset();
    if (FAILED(hr)) return hr;
    hr = video_cmd_->Reset(video_allocator_.Get());
    if (FAILED(hr)) return hr;

    if (dpb_states_[current_slot] != D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE) {
      D3D12_RESOURCE_BARRIER barriers[2] = {};
      for (auto& barrier : barriers) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = dpb_texture_.Get();
        barrier.Transition.StateBefore = dpb_states_[current_slot];
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
      }
      barriers[0].Transition.Subresource = current_slot;
      barriers[1].Transition.Subresource = dpb_slot_count_ + current_slot;
      video_cmd_->ResourceBarrier(2, barriers);
      dpb_states_[current_slot] = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
    }
    if (reference_only_) {
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = output_texture_.Get();
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      video_cmd_->ResourceBarrier(1, &barrier);
    }

    D3D12_VIDEO_DECODE_INPUT_STREAM_ARGUMENTS input = {};
    D3D12_VIDEO_DECODE_OUTPUT_STREAM_ARGUMENTS output = {};
    if (reference_only_) {
      output.pOutputTexture2D = output_texture_.Get();
      output.OutputSubresource = 0;
      output.ConversionArguments.Enable = TRUE;
      output.ConversionArguments.pReferenceTexture2D = dpb_texture_.Get();
      output.ConversionArguments.ReferenceSubresource = current_slot;
    } else {
      output.pOutputTexture2D = dpb_texture_.Get();
      output.OutputSubresource = current_slot;
    }

    ID3D12Resource* reference_frames[17] = {};
    UINT reference_subresources[17] = {};
    for (uint32_t i = 0; i < dpb_slot_count_; ++i) {
      reference_frames[i] = dpb_texture_.Get();
      reference_subresources[i] = i;
    }
    input.ReferenceFrames.NumTexture2Ds = dpb_slot_count_;
    input.ReferenceFrames.ppTexture2Ds = reference_frames;
    input.ReferenceFrames.pSubresources = reference_subresources;
    input.CompressedBitstream.pBuffer = bitstream_buffer_.Get();
    input.CompressedBitstream.Offset = 0;
    input.CompressedBitstream.Size = parsed.bitstream.size();
    input.pHeap = decoder_heap_.Get();

    DXVA_PicParams_H264 pic = {};
    dx_h264::fillPicParams(sps, pps, parsed.slice, parsed, current_slot, reference_usage_, dpb_, feedback_++, pic);
    DXVA_Qmatrix_H264 qmatrix = {};
    dx_h264::fillQMatrix(sps, pps, qmatrix);
    std::vector<DXVA_Slice_H264_Short> slices(parsed.slice_offsets.size());
    for (size_t i = 0; i < parsed.slice_offsets.size(); ++i) {
      const uint32_t begin = parsed.slice_offsets[i];
      const uint32_t end = (i + 1 < parsed.slice_offsets.size()) ? parsed.slice_offsets[i + 1] : static_cast<uint32_t>(parsed.bitstream.size());
      slices[i].BSNALunitDataLocation = begin;
      slices[i].SliceBytesInBuffer = end - begin;
    }
    input.FrameArguments[input.NumFrameArguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_PICTURE_PARAMETERS, sizeof(pic), &pic};
    input.FrameArguments[input.NumFrameArguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_INVERSE_QUANTIZATION_MATRIX, sizeof(qmatrix), &qmatrix};
    input.FrameArguments[input.NumFrameArguments++] = {D3D12_VIDEO_DECODE_ARGUMENT_TYPE_SLICE_CONTROL, static_cast<UINT>(slices.size() * sizeof(DXVA_Slice_H264_Short)), slices.data()};

    video_cmd_->DecodeFrame(decoder_.Get(), &output, &input);

    if (reference_only_) {
      D3D12_RESOURCE_BARRIER barrier = {};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = output_texture_.Get();
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
      barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      video_cmd_->ResourceBarrier(1, &barrier);
    } else {
      D3D12_RESOURCE_BARRIER barriers[2] = {};
      for (auto& barrier : barriers) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = dpb_texture_.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
      }
      barriers[0].Transition.Subresource = current_slot;
      barriers[1].Transition.Subresource = dpb_slot_count_ + current_slot;
      video_cmd_->ResourceBarrier(2, barriers);
      dpb_states_[current_slot] = D3D12_RESOURCE_STATE_COMMON;
    }

    hr = video_cmd_->Close();
    if (FAILED(hr)) return hr;
    ID3D12CommandList* lists[] = {video_cmd_.Get()};
    video_queue_->ExecuteCommandLists(1, lists);
    return signal(video_queue_.Get(), video_fence_.Get(), video_fence_value_, video_event_);
  }

  auto download(uint32_t current_slot) -> std::optional<Picture> {
    if (FAILED(wait(video_fence_.Get(), video_fence_value_, video_event_))) return std::nullopt;

    ID3D12Resource* src = reference_only_ ? output_texture_.Get() : dpb_texture_.Get();
    const UINT src_subresource_y = reference_only_ ? 0 : current_slot;
    const UINT src_subresource_uv = reference_only_ ? 1 : dpb_slot_count_ + current_slot;

    D3D12_RESOURCE_DESC single = src->GetDesc();
    single.DepthOrArraySize = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2] = {};
    UINT rows[2] = {};
    UINT64 row_sizes[2] = {};
    UINT64 total = 0;
    device_->GetCopyableFootprints(&single, 0, 2, 0, footprints, rows, row_sizes, &total);

    D3D12_RESOURCE_DESC readback_desc = {};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    readback_desc.Width = total;
    readback_desc.Height = 1;
    readback_desc.DepthOrArraySize = 1;
    readback_desc.MipLevels = 1;
    readback_desc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES readback_heap = {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(device_->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return std::nullopt;

    if (FAILED(copy_allocator_->Reset())) return std::nullopt;
    if (FAILED(copy_cmd_->Reset(copy_allocator_.Get(), nullptr))) return std::nullopt;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = src;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    copy_cmd_->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.pResource = readback.Get();
    D3D12_TEXTURE_COPY_LOCATION loc_src = {};
    loc_src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    loc_src.pResource = src;
    dst.PlacedFootprint = footprints[0];
    loc_src.SubresourceIndex = src_subresource_y;
    copy_cmd_->CopyTextureRegion(&dst, 0, 0, 0, &loc_src, nullptr);
    dst.PlacedFootprint = footprints[1];
    loc_src.SubresourceIndex = src_subresource_uv;
    copy_cmd_->CopyTextureRegion(&dst, 0, 0, 0, &loc_src, nullptr);

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    copy_cmd_->ResourceBarrier(1, &barrier);
    if (FAILED(copy_cmd_->Close())) return std::nullopt;
    ID3D12CommandList* lists[] = {copy_cmd_.Get()};
    copy_queue_->ExecuteCommandLists(1, lists);
    if (FAILED(signal(copy_queue_.Get(), copy_fence_.Get(), copy_fence_value_, copy_event_))) return std::nullopt;
    if (FAILED(wait(copy_fence_.Get(), copy_fence_value_, copy_event_))) return std::nullopt;

    uint8_t* data = nullptr;
    if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void**>(&data)))) return std::nullopt;
    Picture pic(OM_FORMAT_NV12, width_, height_);
    const auto y_stride = pic.planes.getLinesize(0);
    const auto uv_stride = pic.planes.getLinesize(1);
    auto* y = pic.planes.getData(0);
    auto* uv = pic.planes.getData(1);
    const uint8_t* src_y = data + footprints[0].Offset;
    const uint8_t* src_uv = data + footprints[1].Offset;
    for (uint32_t row = 0; row < height_; ++row) std::memcpy(y + static_cast<size_t>(row) * y_stride, src_y + static_cast<size_t>(row) * footprints[0].Footprint.RowPitch, width_);
    for (uint32_t row = 0; row < (height_ + 1) / 2; ++row) std::memcpy(uv + static_cast<size_t>(row) * uv_stride, src_uv + static_cast<size_t>(row) * footprints[1].Footprint.RowPitch, width_);
    readback->Unmap(0, nullptr);
    return pic;
  }

  static auto signal(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t& value, HANDLE) -> HRESULT {
    ++value;
    return queue->Signal(fence, value);
  }

  static auto wait(ID3D12Fence* fence, uint64_t value, HANDLE event) -> HRESULT {
    if (fence->GetCompletedValue() >= value) return S_OK;
    HRESULT hr = fence->SetEventOnCompletion(value, event);
    if (FAILED(hr)) return hr;
    return WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0 ? S_OK : E_FAIL;
  }

  void release() {
    initialized_ = false;
    if (bitstream_buffer_ && bitstream_ptr_) bitstream_buffer_->Unmap(0, nullptr);
    bitstream_ptr_ = nullptr;
    output_texture_.Reset();
    dpb_texture_.Reset();
    bitstream_buffer_.Reset();
    decoder_heap_.Reset();
    decoder_.Reset();
    video_cmd_.Reset();
    copy_cmd_.Reset();
    video_allocator_.Reset();
    copy_allocator_.Reset();
    video_queue_.Reset();
    copy_queue_.Reset();
    video_fence_.Reset();
    copy_fence_.Reset();
    video_device_.Reset();
    device_.Reset();
    if (video_event_) CloseHandle(video_event_);
    if (copy_event_) CloseHandle(copy_event_);
    video_event_ = nullptr;
    copy_event_ = nullptr;
  }
};

const CodecDescriptor CODEC_DX12_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "dx12_h264",
    .long_name = "DirectX12 H.264 Decoder",
    .vendor = "Microsoft",
    .flags = HARDWARE,
    .caps = CodecCaps{.profiles = {OM_PROFILE_H264_BASELINE, OM_PROFILE_H264_MAIN, OM_PROFILE_H264_HIGH}},
    .decoder_factory = [] { return std::make_unique<DX12Decoder>(); },
};

} // namespace openmedia
