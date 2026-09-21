#include <zlib.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <image/png_common.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/metadata_keys.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <util/io_util.hpp>
#include <util/still_image_muxer.hpp>
#include <vector>

namespace openmedia {

namespace {

constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

// A PNG chunk: length, four-byte type, payload, CRC of type and payload.
constexpr size_t CHUNK_OVERHEAD = 12;
constexpr size_t IHDR_SIZE = 13;
constexpr size_t ACTL_SIZE = 8;
constexpr size_t FCTL_SIZE = 26;

struct Chunk {
  const char* type; // four bytes inside the frame, not NUL terminated
  std::span<const uint8_t> data;

  auto is(const char (&name)[5]) const -> bool { return memcmp(type, name, 4) == 0; }
};

void appendChunk(std::vector<uint8_t>& out, const char* type, std::span<const uint8_t> data) {
  append_u32_be(out, static_cast<uint32_t>(data.size()));
  const size_t crc_begin = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  const auto crc = static_cast<uint32_t>(
      crc32(crc32(0L, nullptr, 0), out.data() + crc_begin, static_cast<uInt>(out.size() - crc_begin)));
  append_u32_be(out, crc);
}

auto parseChunks(std::span<const uint8_t> file, std::vector<Chunk>& out) -> bool {
  if (file.size() < sizeof(PNG_SIGNATURE) ||
      memcmp(file.data(), PNG_SIGNATURE, sizeof(PNG_SIGNATURE)) != 0) {
    return false;
  }

  size_t pos = sizeof(PNG_SIGNATURE);
  while (pos + CHUNK_OVERHEAD <= file.size()) {
    const uint32_t length = load_u32_be(file.data() + pos);
    // The PNG spec caps a chunk at 2^31-1, which also keeps the sum below from
    // overflowing on any target with a 64-bit size_t.
    if (length > 0x7FFFFFFFu || pos + CHUNK_OVERHEAD + length > file.size()) {
      return false;
    }
    out.push_back({reinterpret_cast<const char*>(file.data() + pos + 4),
                   file.subspan(pos + 8, length)});
    pos += CHUNK_OVERHEAD + length;
  }
  return pos == file.size();
}

auto findChunk(const std::vector<Chunk>& chunks, const char (&name)[5]) -> const Chunk* {
  for (const auto& chunk : chunks) {
    if (chunk.is(name)) return &chunk;
  }
  return nullptr;
}

auto sameChunk(const std::vector<Chunk>& a, const std::vector<Chunk>& b, const char (&name)[5]) -> bool {
  const Chunk* lhs = findChunk(a, name);
  const Chunk* rhs = findChunk(b, name);
  if ((lhs == nullptr) != (rhs == nullptr)) return false;
  if (lhs == nullptr) return true;
  return lhs->data.size() == rhs->data.size() &&
         memcmp(lhs->data.data(), rhs->data.data(), lhs->data.size()) == 0;
}

} // namespace

class PNGMuxer final : public StillImageMuxer {
public:
  PNGMuxer()
      : StillImageMuxer(OM_CODEC_PNG) {}

  ~PNGMuxer() override { StillImageMuxer::close(); }

protected:
  auto acceptsMultipleFrames() const -> bool override { return true; }

  auto writeFrames() -> OMError override {
    // A lone image is already the finished file; only an animation needs the
    // APNG chunks layered on top.
    if (frames_.size() == 1) {
      return StillImageMuxer::writeFrames();
    }
    return writeAnimation();
  }

private:
  auto writeAnimation() -> OMError {
    std::vector<Chunk> first;
    if (!parseChunks(frames_.front().bytes, first)) {
      return OM_FORMAT_INVALID_PACKET;
    }

    const Chunk* ihdr = findChunk(first, "IHDR");
    if (ihdr == nullptr || ihdr->data.size() != IHDR_SIZE) {
      return OM_FORMAT_INVALID_PACKET;
    }
    const uint32_t width = load_u32_be(ihdr->data.data());
    const uint32_t height = load_u32_be(ihdr->data.data() + 4);

    std::vector<uint8_t> out;
    out.insert(out.end(), std::begin(PNG_SIGNATURE), std::end(PNG_SIGNATURE));

    // fcTL and fdAT share one counter, in the order the chunks are written.
    uint32_t sequence = 0;

    appendChunk(out, "IHDR", ihdr->data);
    appendChunk(out, "acTL", animationControl());

    // The first frame stays an ordinary IDAT; the fcTL in front of it is what
    // folds it into the animation. Everything else frame 0 carries - palette,
    // colour chunks - describes the whole file and is copied through.
    bool first_frame_declared = false;
    for (const auto& chunk : first) {
      if (chunk.is("IHDR") || chunk.is("IEND")) continue;
      if (chunk.is("IDAT") && !first_frame_declared) {
        appendChunk(out, "fcTL", frameControl(sequence++, width, height, 0));
        first_frame_declared = true;
      }
      appendChunk(out, chunk.type, chunk.data);
    }
    if (!first_frame_declared) {
      return OM_FORMAT_INVALID_PACKET; // no image data
    }

    for (size_t i = 1; i < frames_.size(); ++i) {
      std::vector<Chunk> chunks;
      if (!parseChunks(frames_[i].bytes, chunks)) {
        return OM_FORMAT_INVALID_PACKET;
      }
      // Later frames contribute pixels only: they are decoded against the
      // header and palette written above, so a mismatch cannot be muxed.
      const Chunk* header = findChunk(chunks, "IHDR");
      if (header == nullptr || header->data.size() != IHDR_SIZE ||
          memcmp(header->data.data(), ihdr->data.data(), IHDR_SIZE) != 0) {
        return OM_FORMAT_INVALID_PACKET;
      }
      if (!sameChunk(first, chunks, "PLTE") || !sameChunk(first, chunks, "tRNS")) {
        return OM_FORMAT_INVALID_PACKET;
      }

      appendChunk(out, "fcTL", frameControl(sequence++, width, height, i));

      bool has_data = false;
      for (const auto& chunk : chunks) {
        if (!chunk.is("IDAT")) continue;
        // fdAT is an IDAT with a sequence number in front of it.
        std::vector<uint8_t> payload;
        payload.reserve(4 + chunk.data.size());
        append_u32_be(payload, sequence++);
        payload.insert(payload.end(), chunk.data.begin(), chunk.data.end());
        appendChunk(out, "fdAT", payload);
        has_data = true;
      }
      if (!has_data) {
        return OM_FORMAT_INVALID_PACKET;
      }
    }

    appendChunk(out, "IEND", {});
    return writeAll(out) ? OM_SUCCESS : OM_IO_WRITE_FAILED;
  }

  auto animationControl() const -> std::array<uint8_t, ACTL_SIZE> {
    // num_plays counts repeats with 0 meaning forever, the same as the key.
    const auto plays = static_cast<uint32_t>(
        std::max(0, tracks_.front().metadata.getInt32(ANIMATION_LOOP_COUNT, 0)));

    std::array<uint8_t, ACTL_SIZE> data = {};
    store_u32_be(data.data() + 0, static_cast<uint32_t>(frames_.size()));
    store_u32_be(data.data() + 4, plays);
    return data;
  }

  auto frameControl(uint32_t sequence, uint32_t width, uint32_t height, size_t index) const
      -> std::array<uint8_t, FCTL_SIZE> {
    // Denominator of 1000 keeps the millisecond the rest of the muxer works in;
    // the numerator is 16-bit, which caps a single frame at ~65 seconds.
    const auto delay = static_cast<uint16_t>(std::clamp<int64_t>(frameDurationMs(index), 0, 0xFFFF));

    std::array<uint8_t, FCTL_SIZE> data = {};
    store_u32_be(data.data() + 0, sequence);
    store_u32_be(data.data() + 4, width);
    store_u32_be(data.data() + 8, height);
    store_u32_be(data.data() + 12, 0); // x_offset
    store_u32_be(data.data() + 16, 0); // y_offset
    store_u16_be(data.data() + 20, delay);
    store_u16_be(data.data() + 22, 1000);
    // Every packet is a full-canvas image, so replacing the canvas outright
    // reproduces it exactly; APNG_BLEND_OP_OVER would composite frames with
    // alpha over their predecessor instead.
    data[24] = 0; // dispose_op: APNG_DISPOSE_OP_NONE
    data[25] = 0; // blend_op: APNG_BLEND_OP_SOURCE
    return data;
  }
};

auto createPNGMuxer() -> std::unique_ptr<Muxer> {
  return std::make_unique<PNGMuxer>();
}

} // namespace openmedia
