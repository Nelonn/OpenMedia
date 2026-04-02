#include <comdef.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <windows.h>
#include <wrl/client.h>
#include <codecs.hpp>
#include <iostream>
#include <openmedia/video.hpp>
#include <util/wmf.hpp>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

#ifndef MFSAMPLE_ATTRIBUTE_CLEAN_DATA
#define MFSAMPLE_ATTRIBUTE_CLEAN_DATA {0x93a6a71f, 0x2a95, 0x439b, {0x89, 0x89, 0xeb, 0x2b, 0xf3, 0x77, 0x3f, 0x8a}}
#endif

namespace openmedia {

constexpr Rational MF_TIMEBASE = {1, 10000000};

class WMFVideoDecoder final : public Decoder {
  ComPtr<IMFTransform> decoder_;
  VideoFormat output_format_ = {};
  uint32_t timescale_ = 0;
  bool mf_started_ = false;
  bool initialized_ = false;
  int32_t output_stride_ = 0;
  OMCodecId codec_id_ = OM_CODEC_NONE;

public:
  WMFVideoDecoder() {
    HRESULT hr_co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr_co) && hr_co != RPC_E_CHANGED_MODE) {
      std::cerr << "CoInitializeEx failed: " << hr_co << std::endl;
      return;
    }

    HRESULT hr_mf = MFStartup(MF_VERSION);
    if (FAILED(hr_mf)) {
      std::cerr << "MFStartup failed: " << hr_mf << std::endl;
      return;
    }
    mf_started_ = true;
  }

  ~WMFVideoDecoder() override {
    if (decoder_) {
      decoder_.Reset();
    }
    if (mf_started_) {
      MFShutdown();
    }
  }

  static auto FindDecoder(IMFTransform** ppDecoder, GUID codec) -> HRESULT {
    MFT_REGISTER_TYPE_INFO input_info = {MFMediaType_Video, codec};
    IMFActivate** pp_activates = nullptr;
    UINT32 count = 0;

    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &input_info,
        nullptr,
        &pp_activates,
        &count);

    if (FAILED(hr) || count == 0) {
      return (count == 0) ? E_FAIL : hr;
    }

    hr = pp_activates[0]->ActivateObject(IID_PPV_ARGS(ppDecoder));

    for (UINT32 i = 0; i < count; i++) {
      pp_activates[i]->Release();
    }
    CoTaskMemFree(pp_activates);

    return hr;
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    if (!mf_started_) return OM_CODEC_OPEN_FAILED;

    codec_id_ = options.format.codec_id;
    GUID codec = codecIdToMFVideoFormat(codec_id_);
    if (codec == MFVideoFormat_Base) {
      return OM_CODEC_NOT_FOUND;
    }

    HRESULT hr = FindDecoder(&decoder_, codec);
    if (FAILED(hr)) {
      _com_error err(hr);
      std::cerr << "Failed to find decoder: " << err.ErrorMessage() << std::endl;
      return OM_CODEC_OPEN_FAILED;
    }

    ComPtr<IMFMediaType> input_type;
    MFCreateMediaType(&input_type);
    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, codec);
    MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE,
                         options.format.video.framerate.num, options.format.video.framerate.den);
    MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, options.format.video.width, options.format.video.height);

    //if (!options.extradata.empty()) {
    //  input_type->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, options.extradata.data(), static_cast<UINT32>(options.extradata.size()));
    //}

    timescale_ = (options.time_base.num > 0 && options.time_base.den > 0)
                     ? static_cast<uint32_t>(options.time_base.den / options.time_base.num)
                     : 1;

    hr = decoder_->SetInputType(0, input_type.Get(), 0);
    if (FAILED(hr)) {
      _com_error err(hr);
      std::cerr << "SetInputType failed: " << err.ErrorMessage() << std::endl;
      return OM_CODEC_OPEN_FAILED;
    }

    if (!setup_output_type()) {
      return OM_CODEC_OPEN_FAILED;
    }

    //decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

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

  auto drain_output(std::vector<Frame>& frames) -> OMError {
    while (true) {
      MFT_OUTPUT_STREAM_INFO stream_info = {};
      if (FAILED(decoder_->GetOutputStreamInfo(0, &stream_info))) break;

      MFT_OUTPUT_DATA_BUFFER output_data = {};
      output_data.dwStreamID = 0;

      bool provides_samples = (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES);
      ComPtr<IMFSample> local_sample;

      if (!provides_samples) {
        MFCreateSample(&local_sample);
        ComPtr<IMFMediaBuffer> local_buffer;
        MFCreateMemoryBuffer(stream_info.cbSize, &local_buffer);
        local_sample->AddBuffer(local_buffer.Get());
        output_data.pSample = local_sample.Get();
      }

      DWORD status = 0;
      HRESULT hr = decoder_->ProcessOutput(0, 1, &output_data, &status);

      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        return OM_CODEC_NEED_MORE_DATA; // Normal: loop finished
      }

      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        setup_output_type();
        continue;
      }

      if (FAILED(hr)) return OM_CODEC_DECODE_FAILED;

      // Process the sample and clean up MFT-allocated resources
      if (output_data.pSample) {
        frames.push_back(process_sample(output_data.pSample));

        // If the MFT provided the sample, we MUST release it (MFTEnumEx/ActivateObject rules)
        if (provides_samples) {
          output_data.pSample->Release();
        }
      }

      if (output_data.pEvents) {
        output_data.pEvents->Release();
      }
    }
    return OM_SUCCESS;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    if (!packet.bytes.empty()) {
      HRESULT hr = S_OK;
      ComPtr<IMFSample> input_sample;
      MFCreateSample(&input_sample);

      ComPtr<IMFMediaBuffer> input_buffer;
      MFCreateMemoryBuffer(static_cast<DWORD>(packet.bytes.size()), &input_buffer);

      BYTE* dest = nullptr;
      if (SUCCEEDED(input_buffer->Lock(&dest, nullptr, nullptr))) {
        memcpy(dest, packet.bytes.data(), packet.bytes.size());
        input_buffer->Unlock();
      }
      input_buffer->SetCurrentLength(static_cast<DWORD>(packet.bytes.size()));
      input_sample->AddBuffer(input_buffer.Get());

      LONGLONG hns_time = static_cast<LONGLONG>(packet.pts) * 10000000LL / timescale_;
      input_sample->SetSampleTime(hns_time);

      hr = decoder_->ProcessInput(0, input_sample.Get(), 0);

      // If the decoder is full, we must drain output and try input again
      if (hr == MF_E_NOTACCEPTING) {
        // Drain output first
        auto drain_res = drain_output(frames);
        if (drain_res != OM_SUCCESS) return Err(drain_res);

        // Try input one more time
        hr = decoder_->ProcessInput(0, input_sample.Get(), 0);
      }

      if (FAILED(hr)) {
        return Err(OM_CODEC_DECODE_FAILED);
      }
    } else {
      // This is a "Flush/Drain" signal (end of stream)
      decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }

    while (true) {
      MFT_OUTPUT_STREAM_INFO stream_info = {};
      if (FAILED(decoder_->GetOutputStreamInfo(0, &stream_info))) {
        break;
      }

      MFT_OUTPUT_DATA_BUFFER output_data = {};
      output_data.dwStreamID = 0;

      bool provides_samples = (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES);

      ComPtr<IMFSample> local_sample;
      if (!provides_samples) {
        MFCreateSample(&local_sample);
        ComPtr<IMFMediaBuffer> local_buffer;
        MFCreateMemoryBuffer(stream_info.cbSize, &local_buffer);
        local_sample->AddBuffer(local_buffer.Get());
        output_data.pSample = local_sample.Get();
      }

      DWORD status = 0;
      HRESULT hr = decoder_->ProcessOutput(0, 1, &output_data, &status);

      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        return Err(OM_CODEC_NEED_MORE_DATA);
      }

      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        setup_output_type();
        continue;
      }

      if (FAILED(hr)) break;

      if (output_data.pSample) {
        frames.push_back(process_sample(output_data.pSample));

        if (provides_samples && output_data.pSample) {
          output_data.pSample->Release();
        }
      }

      if (output_data.pEvents) {
        output_data.pEvents->Release();
      }
    }

    if (!frames.empty()) {
      return Ok(std::move(frames));
    }

    return Ok(std::move(frames));
  }

  auto process_sample(IMFSample* sample) -> Frame {
    LONGLONG hns_time = 0;
    sample->GetSampleTime(&hns_time);
    int64_t pts = hns_time * static_cast<LONGLONG>(timescale_) / 10000000LL;

    ComPtr<IMFMediaBuffer> buffer;
    sample->GetBufferByIndex(0, &buffer);

    Picture pic(output_format_.format, output_format_.width, output_format_.height);

    BYTE* data = nullptr;
    DWORD current_len = 0;
    if (SUCCEEDED(buffer->Lock(&data, nullptr, &current_len))) {
      // Use the stride obtained during setup_output_type()
      //copy_planes(pic, data, output_stride_);
      buffer->Unlock();
    }

    Frame frame;
    frame.pts = pts;
    frame.data = std::move(pic);
    return frame;
  }

  void flush() override {
    if (decoder_) {
      decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
      decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
      decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
  }

private:
  auto setup_output_type() -> bool {
    ComPtr<IMFMediaType> output_type;
    HRESULT hr;

    const GUID preferences[] = {
        MFVideoFormat_NV12, MFVideoFormat_I420,
        MFVideoFormat_ARGB32, MFVideoFormat_RGB32};

    for (const auto& pref_subtype : preferences) {
      for (DWORD i = 0;; ++i) {
        hr = decoder_->GetOutputAvailableType(0, i, &output_type);
        if (FAILED(hr)) break;

        GUID subtype;
        output_type->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == pref_subtype) {
          hr = decoder_->SetOutputType(0, output_type.Get(), 0);
          if (SUCCEEDED(hr)) {
            if (subtype == MFVideoFormat_NV12) {
              output_format_.format = OM_FORMAT_NV12;
            } else if (subtype == MFVideoFormat_I420) {
              output_format_.format = OM_FORMAT_YUV420P;
            } else if (subtype == MFVideoFormat_ARGB32) {
              output_format_.format = OM_FORMAT_B8G8R8A8;
            } else if (subtype == MFVideoFormat_RGB32) {
              output_format_.format = OM_FORMAT_R8G8B8A8;
            } else {
              output_format_.format = OM_FORMAT_UNKNOWN;
            }

            UINT32 width = 0, height = 0;
            MFGetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, &width, &height);
            output_format_.width = width;
            output_format_.height = height;

            // FIX (bug 3): read the stride from the media type so we can copy
            // planes correctly even when the decoder adds row padding.
            UINT32 stride_u32 = 0;
            if (SUCCEEDED(output_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride_u32))) {
              output_stride_ = static_cast<int32_t>(stride_u32);
            } else {
              // Attribute absent — compute minimum stride for the chosen format.
              LONG computed = 0;
              if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(
                      subtype.Data1, width, &computed))) {
                output_stride_ = static_cast<int32_t>(computed);
              } else {
                output_stride_ = static_cast<int32_t>(width);
              }
            }

            return true;
          }
        }
      }
    }
    return false;
  }
};

auto create_wmf_video_decoder() -> std::unique_ptr<Decoder> {
  return std::make_unique<WMFVideoDecoder>();
}

const CodecDescriptor CODEC_WMF_VIDEO_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "wmf_h264",
    .long_name = "Windows Media Foundation H.264 decoder",
    .vendor = "Microsoft",
    .flags = NONE,
    .decoder_factory = create_wmf_video_decoder,
};

const CodecDescriptor CODEC_WMF_VIDEO_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "wmf_h265",
    .long_name = "Windows Media Foundation H.265/HEVC decoder",
    .vendor = "Microsoft",
    .flags = NONE,
    .decoder_factory = create_wmf_video_decoder,
};

const CodecDescriptor CODEC_WMF_VIDEO_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "wmf_av1",
    .long_name = "Windows Media Foundation AV1 decoder",
    .vendor = "Microsoft",
    .flags = NONE,
    .decoder_factory = create_wmf_video_decoder,
};

} // namespace openmedia
