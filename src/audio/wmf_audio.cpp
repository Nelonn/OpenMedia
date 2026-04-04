#include <codecs.hpp>
#include <openmedia/audio.hpp>
#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mfobjects.h>
#include <mferror.h>
#include <wrl/client.h>
#include <comdef.h>
#include <vector>
#include <format>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace openmedia {

static auto codecIdToMFCodec(OMCodecId codec) -> GUID {
  switch (codec) {
    case OM_CODEC_FLAC: return MFAudioFormat_FLAC;
    case OM_CODEC_MP3: return MFAudioFormat_MP3;
    case OM_CODEC_AAC: return MFAudioFormat_AAC;
    case OM_CODEC_OPUS: return MFAudioFormat_Opus;
    case OM_CODEC_VORBIS: return MFAudioFormat_Vorbis;
    case OM_CODEC_ALAC: return MFAudioFormat_ALAC;
    case OM_CODEC_AC3: return MFAudioFormat_Dolby_AC3;
    case OM_CODEC_EAC3: return MFAudioFormat_Dolby_DDPlus;
    default: return MFAudioFormat_Base;
  }
}

class WMFAudioDecoder final : public Decoder {
  ComPtr<IMFTransform> decoder_;
  LoggerRef logger_;
  AudioFormat output_format_ = {};
  int64_t timescale_ = 0;
  bool initialized_ = false;

public:
  WMFAudioDecoder() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
  }

  ~WMFAudioDecoder() override {
    if (decoder_) {
      decoder_.Reset();
    }
    MFShutdown();
  }

  static auto FindDecoder(IMFTransform** ppDecoder, GUID codec) -> HRESULT {
    MFT_REGISTER_TYPE_INFO inputInfo = { MFMediaType_Audio, codec };
    IMFActivate** ppActivates = nullptr;
    UINT32 count = 0;

    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_AUDIO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputInfo,
        nullptr,
        &ppActivates,
        &count);

    if (FAILED(hr) || count == 0) {
      return (count == 0) ? E_FAIL : hr;
    }

    hr = ppActivates[0]->ActivateObject(IID_PPV_ARGS(ppDecoder));

    for (UINT32 i = 0; i < count; i++) {
      ppActivates[i]->Release();
    }
    CoTaskMemFree(ppActivates);

    return hr;
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    logger_ = options.logger ? options.logger : Logger::refDefault();

    GUID codec = codecIdToMFCodec(options.format.codec_id);
    if (codec == MFAudioFormat_Base) {
      return OM_CODEC_NOT_FOUND;
    }

    HRESULT hr = FindDecoder(&decoder_, codec);
    if (FAILED(hr)) {
      _com_error err(hr);
      logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, std::format("Failed to find decoder: {}", err.ErrorMessage()));
      return OM_CODEC_OPEN_FAILED;
    }

    ComPtr<IMFMediaType> input_type;
    MFCreateMediaType(&input_type);
    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    input_type->SetGUID(MF_MT_SUBTYPE, codec);
    input_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, options.format.audio.channels);
    input_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, options.format.audio.sample_rate);
    input_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, options.format.audio.bit_depth);
    input_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);

    if (options.format.codec_id == OM_CODEC_AAC) {
      uint32_t payload_type = options.extradata.empty() ? 1 : 0;
      input_type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, payload_type);
      input_type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, static_cast<uint32_t>(options.format.profile));
      /*uint32_t avg = options.format.bitrate / 8;
      if (avg == 0) avg = 20000;
      input_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, avg);*/

      if (!options.extradata.empty()) {
        std::vector<uint8_t> user_data(12 + options.extradata.size());
        struct {
          uint16_t wPayloadType;
          uint16_t wAudioProfileLevelIndication;
          uint16_t wStructType;
          uint16_t wReserved1;
          uint32_t dwReserved2;
        } aac_info = { static_cast<uint16_t>(payload_type), 0xFE, 0, 0, 0 };
        memcpy(user_data.data(), &aac_info, 12);
        memcpy(user_data.data() + 12, options.extradata.data(), options.extradata.size());
        input_type->SetBlob(MF_MT_USER_DATA, user_data.data(), static_cast<UINT32>(user_data.size()));
      }
    } else {
      /*if (options.format.bitrate > 0) {
        input_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, options.format.bitrate / 8);
      }*/
      if (!options.extradata.empty()) {
        input_type->SetBlob(MF_MT_USER_DATA, options.extradata.data(), static_cast<UINT32>(options.extradata.size()));
      }
    }

    timescale_ = 10000000LL * options.time_base.num / options.time_base.den;

    hr = decoder_->SetInputType(0, input_type.Get(), 0);
    if (FAILED(hr)) {
      _com_error err(hr);
      logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, std::format("SetInputType failed: {}", err.ErrorMessage()));
      return OM_CODEC_OPEN_FAILED;
    }

    if (!setupOutputType()) {
      return OM_CODEC_OPEN_FAILED;
    }

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;

    DecodingInfo info;
    info.media_type = OM_MEDIA_AUDIO;
    info.audio_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    if (!packet.bytes.empty()) {
      ComPtr<IMFSample> sample;
      MFCreateSample(&sample);

      ComPtr<IMFMediaBuffer> buffer;
      MFCreateMemoryBuffer(static_cast<DWORD>(packet.bytes.size()), &buffer);

      BYTE* dest = nullptr;
      buffer->Lock(&dest, nullptr, nullptr);
      memcpy(dest, packet.bytes.data(), packet.bytes.size());
      buffer->Unlock();
      buffer->SetCurrentLength(static_cast<DWORD>(packet.bytes.size()));

      sample->AddBuffer(buffer.Get());

      LONGLONG hns_sample_time = packet.pts * timescale_;
      LONGLONG hns_sample_duration = packet.duration * timescale_;

      sample->SetSampleTime(hns_sample_time);
      sample->SetSampleDuration(hns_sample_duration);

      HRESULT hr = decoder_->ProcessInput(0, sample.Get(), 0);
      if (FAILED(hr)) {
        _com_error err(hr);
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, std::format("ProcessInput failed: {}", err.ErrorMessage()));
        return Err(OM_CODEC_DECODE_FAILED);
      }
    }

    while (true) {
      MFT_OUTPUT_STREAM_INFO info;
      HRESULT hr = decoder_->GetOutputStreamInfo(0, &info);
      if (FAILED(hr)) {
        _com_error err(hr);
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, std::format("GetOutputStreamInfo failed: {}", err.ErrorMessage()));
        break;
      }

      MFT_OUTPUT_DATA_BUFFER output_buffer = {};
      output_buffer.dwStreamID = 0;

      ComPtr<IMFSample> out_sample;
      bool we_provided_sample = false;
      if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
        MFCreateSample(&out_sample);
        ComPtr<IMFMediaBuffer> out_buffer;
        MFCreateMemoryBuffer(info.cbSize, &out_buffer);
        out_sample->AddBuffer(out_buffer.Get());
        output_buffer.pSample = out_sample.Get();
        we_provided_sample = true;
      }

      DWORD status = 0;
      hr = decoder_->ProcessOutput(0, 1, &output_buffer, &status);

      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        _com_error err(hr);
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_VERBOSE, std::format("ProcessOutput warned: {}", err.ErrorMessage()));
        break;
      }
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        setupOutputType();
        continue;
      }
      if (FAILED(hr)) {
        _com_error err(hr);
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, std::format("ProcessOutput failed: {}", err.ErrorMessage()));
        break;
      }

      if (output_buffer.pSample) {
        ComPtr<IMFMediaBuffer> media_buffer;
        output_buffer.pSample->GetBufferByIndex(0, &media_buffer);

        BYTE* data = nullptr;
        DWORD current_len = 0;
        media_buffer->Lock(&data, nullptr, &current_len);

        size_t bps = getBytesPerSample(output_format_.sample_format);
        uint32_t samples = current_len / (output_format_.channels * static_cast<uint32_t>(bps));
        AudioSamples samples_fmt(output_format_, samples);

        LONGLONG sample_time = 0;
        int64_t pts = 0;
        if (SUCCEEDED(output_buffer.pSample->GetSampleTime(&sample_time))) {
          pts = sample_time * output_format_.sample_rate / 10000000;
        }

        memcpy(samples_fmt.planes.data[0], data, current_len);
        media_buffer->Unlock();

        Frame frame = {};
        frame.pts = pts;
        frame.data = std::move(samples_fmt);
        frames.push_back(std::move(frame));

        if (!we_provided_sample) {
          output_buffer.pSample->Release();
        }
      }

      if (output_buffer.pEvents) {
        output_buffer.pEvents->Release();
      }
    }

    return Ok(std::move(frames));
  }

  void flush() override {
    if (decoder_) {
      decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
  }

private:
  auto setupOutputType() -> bool {
    ComPtr<IMFMediaType> output_type;
    HRESULT hr;

    const GUID preferences[] = { MFAudioFormat_PCM, MFAudioFormat_Float };

    for (const auto& pref_subtype : preferences) {
      for (DWORD i = 0; ; ++i) {
        hr = decoder_->GetOutputAvailableType(0, i, &output_type);
        if (FAILED(hr)) break;

        GUID subtype;
        output_type->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == pref_subtype) {
          hr = decoder_->SetOutputType(0, output_type.Get(), 0);
          if (SUCCEEDED(hr)) {
            if (subtype == MFAudioFormat_PCM) {
              output_format_.sample_format = OM_SAMPLE_S16;
            } else {
              output_format_.sample_format = OM_SAMPLE_F32;
            }
            output_type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &output_format_.sample_rate);
            output_type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &output_format_.channels);

            return true;
          }
        }
      }
    }
    return false;
  }
};

auto create_wmf_audio_decoder() -> std::unique_ptr<Decoder> {
  return std::make_unique<WMFAudioDecoder>();
}

const CodecDescriptor CODEC_WMF_AAC = {
  .codec_id = OM_CODEC_AAC,
  .type = OM_MEDIA_AUDIO,
  .name = "wmf_aac",
  .long_name = "AAC (Windows Media Foundation)",
  .vendor = "Microsoft",
  .flags = NONE,
  .decoder_factory = create_wmf_audio_decoder,
};

const CodecDescriptor CODEC_WMF_MP3 = {
  .codec_id = OM_CODEC_MP3,
  .type = OM_MEDIA_AUDIO,
  .name = "wmf_mp3",
  .long_name = "MP3 (Windows Media Foundation)",
  .vendor = "Microsoft",
  .flags = NONE,
  .decoder_factory = create_wmf_audio_decoder,
};

} // namespace openmedia
