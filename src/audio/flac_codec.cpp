#include <codecs.hpp>
#include <openmedia/audio.hpp>
#include <FLAC/stream_decoder.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <iostream>
#include <format>

namespace openmedia {

class FLACDecoder final : public Decoder {
  FLAC__StreamDecoder* decoder_ = nullptr;
  std::vector<Frame> decoded_frames_;
  std::vector<uint8_t> init_header_;
  const Packet* current_packet_ = nullptr;
  size_t packet_offset_ = 0;
  uint8_t packet_reads_ = 0;
  uint64_t packet_sample_offset_ = 0;
  std::optional<AudioFormat> output_format_;
  LoggerRef logger_ = {};

public:
  FLACDecoder() {
    decoder_ = FLAC__stream_decoder_new();
  }

  ~FLACDecoder() override {
    if (decoder_) {
      FLAC__stream_decoder_delete(decoder_);
    }
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_FLAC) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.extradata.empty()) {
      return OM_CODEC_INVALID_PARAMS;
    }
    logger_ = options.logger ? options.logger : Logger::refDefault();

    auto status = FLAC__stream_decoder_init_stream(
        decoder_,
        read_callback,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        write_callback,
        nullptr,
        error_callback,
        this);

    if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
      return OM_CODEC_OPEN_FAILED;
    }

    init_header_ = std::vector<uint8_t>(options.extradata.begin(), options.extradata.end());

    Packet p;
    p.allocate(init_header_.size());
    std::memcpy(p.buffer->bytes().data(), init_header_.data(), init_header_.size());
    p.pts = 0;
    current_packet_ = &p;
    packet_offset_ = 0;

    if (!FLAC__stream_decoder_process_until_end_of_metadata(decoder_)) {
      current_packet_ = nullptr;
      return OM_CODEC_OPEN_FAILED;
    }

    current_packet_ = nullptr;
    packet_offset_ = 0;

    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!output_format_.has_value()) return std::nullopt;
    return {};
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    current_packet_ = &packet;
    packet_offset_ = 0;
    packet_reads_ = 0;
    packet_sample_offset_ = 0;
    decoded_frames_.clear();

    FLAC__stream_decoder_process_single(decoder_);

    current_packet_ = nullptr;
    packet_offset_ = 0;

    return Ok(std::move(decoded_frames_));
  }

  void flush() override {
    FLAC__stream_decoder_flush(decoder_);
  }

private:
  static auto read_callback(
      const FLAC__StreamDecoder* /*decoder*/,
      FLAC__byte buffer[], size_t* bytes,
      void* client_data) -> FLAC__StreamDecoderReadStatus {
    auto* self = static_cast<FLACDecoder*>(client_data);
    self->packet_reads_++;
    if (!self->current_packet_ ||
        self->packet_offset_ >= self->current_packet_->bytes.size()) {
      *bytes = 0;
      return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

    size_t to_copy = std::min(*bytes,
                              self->current_packet_->bytes.size() - self->packet_offset_);
    memcpy(buffer,
           self->current_packet_->bytes.data() + self->packet_offset_,
           to_copy);
    self->packet_offset_ += to_copy;
    *bytes = to_copy;

    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
  }

  static auto write_callback(
      const FLAC__StreamDecoder* /*decoder*/,
      const FLAC__Frame* frame,
      const FLAC__int32* const buffer[],
      void* client_data) -> FLAC__StreamDecoderWriteStatus {
    auto* self = static_cast<FLACDecoder*>(client_data);

    const uint32_t nb_samples = frame->header.blocksize;

    AudioFormat fmt = {};
    fmt.sample_format = OM_SAMPLE_S32;
    fmt.bits_per_sample = frame->header.bits_per_sample;
    fmt.sample_rate = frame->header.sample_rate;
    fmt.channels = frame->header.channels;
    fmt.planar = true;

    AudioSamples samples(fmt, nb_samples);
    samples.bits_per_sample = fmt.bits_per_sample;

    for (unsigned c = 0; c < fmt.channels; ++c) {
      const FLAC__int32* src = buffer[c];
      int32_t* dst = reinterpret_cast<int32_t*>(samples.planes.data[c]);
      memcpy(dst, src, nb_samples * sizeof(int32_t));
    }

    Frame audio_frame;
    audio_frame.pts = self->current_packet_->pts + static_cast<int64_t>(self->packet_sample_offset_);
    audio_frame.data = std::move(samples);
    self->packet_sample_offset_ += static_cast<uint64_t>(frame->header.blocksize);

    self->decoded_frames_.push_back(std::move(audio_frame));

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }

  static void error_callback(
      const FLAC__StreamDecoder* /*decoder*/,
      FLAC__StreamDecoderErrorStatus status,
      void* client_data) {
    auto* self = static_cast<FLACDecoder*>(client_data);
    if (self->logger_) {
      auto str = std::format("FLAC decoder error: {}", FLAC__StreamDecoderErrorStatusString[status]);
      self->logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, str);
    }
  }
};

const CodecDescriptor CODEC_FLAC = {
  .codec_id = OM_CODEC_FLAC,
  .type = OM_MEDIA_AUDIO,
  .name = "flac",
  .long_name = "Free Lossless Audio Codec",
  .vendor = "Xiph.Org",
  .flags = NONE,
  .decoder_factory = []{ return std::make_unique<FLACDecoder>(); },
};

} // namespace openmedia
