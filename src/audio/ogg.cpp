#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/track.hpp>
#include <openmedia/packet.hpp>
#include <ogg/ogg.h>
#include <future>
#include <cstring>
#include <map>

namespace openmedia {

class OggDemuxer final : public BaseDemuxer {
    ogg_sync_state sync_ = {};
    std::map<int, ogg_stream_state> streams_state_;
    std::map<int, int> stream_id_to_index_;
    std::map<int, bool> streams_header_complete_;

public:
    OggDemuxer() {
        ogg_sync_init(&sync_);
    }

    ~OggDemuxer() override {
        for (auto& pair : streams_state_) {
            ogg_stream_clear(&pair.second);
        }
        ogg_sync_clear(&sync_);
    }

    auto open(std::unique_ptr<InputStream> input) -> OMError override {
        input_ = std::move(input);
        if (!input_ || !input_->isValid()) {
            return OM_IO_INVALID_STREAM;
        }

        while (tracks_.empty() || !all_headers_read()) {
            if (!read_more_and_process()) break;
            if (input_->isEOF() && tracks_.empty()) break;
        }

        return tracks_.empty() ? OM_FORMAT_PARSE_FAILED : OM_SUCCESS;
    }

    auto readPacket() -> Result<Packet, OMError> override {
        while (true) {
            for (auto& pair : streams_state_) {
                ogg_packet op;
                if (ogg_stream_packetout(&pair.second, &op) == 1) {
                    Packet pkt;
                    pkt.allocate(op.bytes);
                    memcpy(pkt.bytes.data(), op.packet, op.bytes);
                    pkt.stream_index = stream_id_to_index_[pair.first];
                    pkt.pts = op.granulepos;
                    pkt.dts = pkt.pts;
                    return Ok(std::move(pkt));
                }
            }
            if (!read_more_and_process()) return Err(OM_FORMAT_PARSE_FAILED);
        }
    }

    auto seek(int64_t timestamp_us, SeekMode mode) -> OMError override {
        if (timestamp_us == 0) {
            input_->seek(0, Whence::BEG);
            ogg_sync_reset(&sync_);
            for (auto& pair : streams_state_) {
                ogg_stream_reset(&pair.second);
            }
            return OM_SUCCESS;
        }
        return OM_FORMAT_PARSE_FAILED;
    }

private:
    auto read_more_and_process() -> bool {
        char* buffer = ogg_sync_buffer(&sync_, 8192);
        size_t n = input_->read({reinterpret_cast<uint8_t*>(buffer), 8192});
        if (n == 0) return false;

        ogg_sync_wrote(&sync_, n);

        ogg_page og;
        while (ogg_sync_pageout(&sync_, &og) == 1) {
            int serial = ogg_page_serialno(&og);
            if (!streams_state_.contains(serial)) {
                ogg_stream_init(&streams_state_[serial], serial);
                
                Track track;
                track.index = static_cast<int32_t>(tracks_.size());
                track.id = serial;
                stream_id_to_index_[serial] = track.index;
                
                ogg_stream_pagein(&streams_state_[serial], &og);
                
                ogg_packet op;
                if (ogg_stream_packetpeek(&streams_state_[serial], &op) == 1) {
                    if (op.bytes >= 7 && memcmp(op.packet + 1, "vorbis", 6) == 0) {
                        track.format.type = OM_MEDIA_AUDIO;
                        track.format.codec_id = OM_CODEC_VORBIS;
                        if (op.bytes >= 30) {
                            track.format.audio.channels = op.packet[11];
                            track.format.audio.sample_rate = load_u32_le(op.packet + 12);
                            track.bitrate = load_u32_le(op.packet + 20);
                            track.time_base = {1, static_cast<int32_t>(track.format.audio.sample_rate)};
                        }
                    } else if (op.bytes >= 8 && memcmp(op.packet, "OpusHead", 8) == 0) {
                        track.format.type = OM_MEDIA_AUDIO;
                        track.format.codec_id = OM_CODEC_OPUS;
                        if (op.bytes >= 19) {
                            track.format.audio.channels = op.packet[9];
                            track.format.audio.sample_rate = load_u32_le(op.packet + 12);
                            track.time_base = {1, 48000};
                        }
                    }
                }
                tracks_.push_back(track);
                streams_header_complete_[serial] = true;
            } else {
                ogg_stream_pagein(&streams_state_[serial], &og);
            }
        }
        return true;
    }

    auto all_headers_read() const -> bool {
        if (tracks_.empty()) return false;
        for (const auto& stream : tracks_) {
            if (!streams_header_complete_.contains(stream.id)) return false;
        }
        return true;
    }
};

auto create_ogg_demuxer() -> std::unique_ptr<Demuxer> {
    return std::make_unique<OggDemuxer>();
}

const FormatDescriptor FORMAT_OGG = {
    .container_id = OM_CONTAINER_OGG,
    .name = "ogg",
    .long_name = "Ogg",
    .demuxer_factory = [] { return create_ogg_demuxer(); },
    .muxer_factory = {},
};

}
