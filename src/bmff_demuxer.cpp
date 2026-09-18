#include <algorithm>
#include <annexb.hpp>
#include <nal_config.hpp>
#include <cstdint>
#include <cstring>
#include <optional>
#include <openmedia/audio.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/io.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <openmedia/metadata_keys.hpp>
#include <span>
#include <string>
#include <util/bit_reader.hpp>
#include <util/byte_reader.hpp>
#include <util/color_codes.hpp>
#include <util/date_time.hpp>
#include <util/demuxer_base.hpp>
#include <util/id3_genres.hpp>
#include <util/io_util.hpp>
#include <vector>

namespace openmedia {
namespace {

static constexpr uint32_t AAC_SAMPLE_RATES[] = {
    96000, 88200, 64000, 48000, 44100, 32000,
    24000, 22050, 16000, 12000, 11025, 8000, 7350};

static constexpr uint8_t AAC_CHANNELS[] = {0, 1, 2, 3, 4, 5, 6, 8};

static consteval auto ATOM(char a, char b, char c, char d) -> uint32_t {
  return magic_u32(a, b, c, d);
}

template<std::size_t N>
static constexpr auto containsAtom(const uint32_t (&table)[N], uint32_t type) -> bool {
  for (const uint32_t t : table) {
    if (t == type) return true;
  }
  return false;
}

static auto isContainerBox(uint32_t type) -> bool {
  static constexpr uint32_t TABLE[] = {
      ATOM('m', 'o', 'o', 'v'),
      ATOM('t', 'r', 'a', 'k'),
      ATOM('e', 'd', 't', 's'),
      ATOM('m', 'd', 'i', 'a'),
      ATOM('m', 'i', 'n', 'f'),
      ATOM('d', 'i', 'n', 'f'),
      ATOM('s', 't', 'b', 'l'),
      ATOM('w', 'a', 'v', 'e'),
      ATOM('s', 'i', 'n', 'f'), // protection scheme information
      ATOM('s', 'c', 'h', 'i'), // scheme information
      // `udta`, `meta` and `ilst` are containers too, but each needs something
      // done on the way in, so handleBox takes them itself.
  };
  return containsAtom(TABLE, type);
}

static auto isIgnoredBox(uint32_t type) -> bool {
  static constexpr uint32_t TABLE[] = {
      ATOM('f', 't', 'y', 'p'),
      ATOM('f', 'r', 'e', 'e'),
      ATOM('s', 'k', 'i', 'p'),
      ATOM('w', 'i', 'd', 'e'),
      ATOM('p', 'n', 'o', 't'),
      ATOM('j', 'P', '2', ' '),
  };
  return containsAtom(TABLE, type);
}

static auto isAvcVariant(uint32_t fmt) -> bool {
  static constexpr uint32_t TABLE[] = {
      ATOM('a', 'v', 'c', '1'),
      ATOM('a', 'v', 'c', '2'),
      ATOM('a', 'v', 'c', '3'),
      ATOM('a', 'v', 'c', '4'),
      ATOM('d', 'v', 'a', '1'),
      ATOM('d', 'v', 'a', 'v'),
      ATOM('H', '2', '6', '4'),
      ATOM('X', '2', '6', '4'),
  };
  return containsAtom(TABLE, fmt);
}

static auto isDolbyVisionAvcVariant(uint32_t fmt) -> bool {
  return fmt == ATOM('d', 'v', 'a', '1') || fmt == ATOM('d', 'v', 'a', 'v');
}

static auto isHevcVariant(uint32_t fmt) -> bool {
  static constexpr uint32_t TABLE[] = {
      ATOM('h', 'v', 'c', '1'),
      ATOM('h', 'e', 'v', '1'),
      ATOM('d', 'v', 'h', '1'),
      ATOM('d', 'v', 'h', 'e'),
      ATOM('H', 'E', 'V', 'C'),
  };
  return containsAtom(TABLE, fmt);
}

static auto isDolbyVisionHevcVariant(uint32_t fmt) -> bool {
  return fmt == ATOM('d', 'v', 'h', '1') || fmt == ATOM('d', 'v', 'h', 'e');
}

static auto isVvcVariant(uint32_t fmt) -> bool {
  static constexpr uint32_t TABLE[] = {
      ATOM('v', 'v', 'c', '1'),
      ATOM('v', 'v', 'i', '1'),
  };
  return containsAtom(TABLE, fmt);
}

static auto isEncryptedEntry(uint32_t fmt) -> bool {
  return fmt == ATOM('e', 'n', 'c', 'v') || fmt == ATOM('e', 'n', 'c', 'a') ||
         fmt == ATOM('e', 'n', 'c', 's');
}

static auto isMp4aVariant(uint32_t fmt) -> bool {
  return fmt == ATOM('m', 'p', '4', 'a') || fmt == ATOM('M', 'P', '4', 'A');
}

static auto isPcmVariant(uint32_t fmt) -> bool {
  static constexpr uint32_t TABLE[] = {
      ATOM('r', 'a', 'w', ' '),
      ATOM('t', 'w', 'o', 's'),
      ATOM('s', 'o', 'w', 't'),
      ATOM('i', 'n', '2', '4'),
      ATOM('i', 'n', '3', '2'),
      ATOM('f', 'l', '3', '2'),
      ATOM('f', 'l', '6', '4'),
  };
  return containsAtom(TABLE, fmt);
}

// QuickTime sound four-CCs carry the sample layout in the code itself, and the
// ones below `sowt` are big-endian by definition. A `wave/enda` box may flip
// that, so endianness is resolved separately and only applied afterwards.
enum class PcmLayout : uint8_t { Unknown, U8, S16, S24, S32, F32, F64 };

static auto pcmLayoutOf(uint32_t fmt) -> PcmLayout {
  switch (fmt) {
    case ATOM('r', 'a', 'w', ' '): return PcmLayout::U8;
    case ATOM('t', 'w', 'o', 's'):
    case ATOM('s', 'o', 'w', 't'): return PcmLayout::S16;
    case ATOM('i', 'n', '2', '4'): return PcmLayout::S24;
    case ATOM('i', 'n', '3', '2'): return PcmLayout::S32;
    case ATOM('f', 'l', '3', '2'): return PcmLayout::F32;
    case ATOM('f', 'l', '6', '4'): return PcmLayout::F64;
    default: return PcmLayout::Unknown;
  }
}

// `sowt` ("twos" reversed) is the only inherently little-endian code; every
// other QuickTime PCM four-CC is big-endian unless `enda` says otherwise.
static auto pcmDefaultLittleEndian(uint32_t fmt) -> bool {
  return fmt == ATOM('s', 'o', 'w', 't') || fmt == ATOM('r', 'a', 'w', ' ');
}

// OM_CODEC_NONE for layouts this codec set has no id for (24-bit, big-endian
// float). Reporting the track as unsupported beats decoding it as noise.
static auto pcmCodecId(PcmLayout layout, bool little_endian) -> OMCodecId {
  switch (layout) {
    case PcmLayout::U8: return OM_CODEC_PCM_U8; // endian-neutral
    case PcmLayout::S16: return little_endian ? OM_CODEC_PCM_S16LE : OM_CODEC_PCM_S16BE;
    case PcmLayout::S32: return little_endian ? OM_CODEC_PCM_S32LE : OM_CODEC_PCM_S32BE;
    case PcmLayout::F32: return little_endian ? OM_CODEC_PCM_F32LE : OM_CODEC_NONE;
    case PcmLayout::F64: return little_endian ? OM_CODEC_PCM_F64LE : OM_CODEC_NONE;
    case PcmLayout::S24:
    case PcmLayout::Unknown:
    default: return OM_CODEC_NONE;
  }
}

static auto pcmBitDepth(PcmLayout layout) -> uint32_t {
  switch (layout) {
    case PcmLayout::U8: return 8;
    case PcmLayout::S16: return 16;
    case PcmLayout::S24: return 24;
    case PcmLayout::S32:
    case PcmLayout::F32: return 32;
    case PcmLayout::F64: return 64;
    default: return 0;
  }
}

// --- AC-3 family, ETSI TS 102 366 -----------------------------------------

static auto ac3SampleRate(uint32_t fscod) -> uint32_t {
  static constexpr uint32_t RATES[] = {48000, 44100, 32000};
  return fscod < std::size(RATES) ? RATES[fscod] : 0u;
}

// acmod names the main audio-service channel arrangement; 0 is dual mono.
static auto ac3Channels(uint32_t acmod) -> uint32_t {
  static constexpr uint8_t CHANNELS[] = {2, 1, 2, 3, 3, 4, 4, 5};
  return acmod < std::size(CHANNELS) ? CHANNELS[acmod] : 0u;
}

static auto ac3BitRateKbps(uint32_t bit_rate_code) -> uint32_t {
  static constexpr uint16_t RATES[] = {32,  40,  48,  56,  64,  80,  96,
                                       112, 128, 160, 192, 224, 256, 320,
                                       384, 448, 512, 576, 640};
  return bit_rate_code < std::size(RATES) ? RATES[bit_rate_code] : 0u;
}

// chan_loc marks which extra channels the dependent substreams add on top of
// the independent substream's own acmod/lfeon arrangement.
static auto eac3DependentChannels(uint32_t chan_loc) -> uint32_t {
  static constexpr uint8_t PER_BIT[] = {
      2, // Lc/Rc
      2, // Lrs/Rrs
      1, // Cs
      1, // Ts
      2, // Lsd/Rsd
      2, // Lw/Rw
      2, // Lvh/Rvh
      1, // Cvh
      1, // LFE2
  };
  uint32_t extra = 0;
  for (size_t bit = 0; bit < std::size(PER_BIT); ++bit) {
    if (chan_loc & (1u << bit)) extra += PER_BIT[bit];
  }
  return extra;
}

static auto isDtsVariant(uint32_t fmt) -> bool {
  static constexpr uint32_t TABLE[] = {
      ATOM('d', 't', 's', 'c'), // core
      ATOM('d', 't', 's', 'e'), // LBR extension only
      ATOM('d', 't', 's', 'h'), // core + extension (HD)
      ATOM('d', 't', 's', 'l'), // lossless extension only
  };
  return containsAtom(TABLE, fmt);
}

// --- Descriptive metadata (iTunes-style `ilst`, and QuickTime `udta`) -------

// The `data` box states how to read its payload. Only the forms that actually
// appear in these containers are listed.
enum : uint32_t {
  DATA_TYPE_IMPLICIT = 0,
  DATA_TYPE_UTF8 = 1,
  DATA_TYPE_UTF16BE = 2,
  DATA_TYPE_JPEG = 13,
  DATA_TYPE_PNG = 14,
  DATA_TYPE_SIGNED_INT = 21,
  DATA_TYPE_UNSIGNED_INT = 22,
  DATA_TYPE_BMP = 27,
};

struct MetadataTag {
  uint32_t atom;
  Key key;
};

// Tags whose payload is text and needs no further interpretation. The leading
// 0xA9 byte is the copyright sign that marks QuickTime's own tag names.
constexpr MetadataTag TEXT_TAGS[] = {
    {ATOM('\xA9', 'n', 'a', 'm'), TITLE},
    {ATOM('\xA9', 'A', 'R', 'T'), ARTIST},
    {ATOM('a', 'A', 'R', 'T'), ALBUM_ARTIST},
    {ATOM('\xA9', 'a', 'l', 'b'), ALBUM},
    {ATOM('\xA9', 'd', 'a', 'y'), DATE},
    {ATOM('\xA9', 'g', 'e', 'n'), GENRE},
    {ATOM('\xA9', 'c', 'm', 't'), COMMENT},
    {ATOM('\xA9', 'w', 'r', 't'), COMPOSER},
    {ATOM('\xA9', 'c', 'o', 'm'), COMPOSER},
    {ATOM('\xA9', 't', 'o', 'o'), ENCODER},
    {ATOM('\xA9', 'l', 'y', 'r'), LYRICS},
    {ATOM('\xA9', 'g', 'r', 'p'), GROUPING},
    {ATOM('c', 'p', 'r', 't'), COPYRIGHT},
    {ATOM('\xA9', 'c', 'p', 'y'), COPYRIGHT},
    {ATOM('d', 'e', 's', 'c'), DESCRIPTION},
    {ATOM('l', 'd', 'e', 's'), DESCRIPTION},
    {ATOM('s', 'o', 'n', 'm'), SORT_TITLE},
    {ATOM('s', 'o', 'a', 'r'), SORT_ARTIST},
    {ATOM('s', 'o', 'a', 'l'), SORT_ALBUM},
    {ATOM('s', 'o', 'a', 'a'), SORT_ALBUM_ARTIST},
};

static auto textMetadataKey(uint32_t atom) -> const Key* {
  for (const MetadataTag& tag : TEXT_TAGS) {
    if (tag.atom == atom) return &tag.key;
  }
  return nullptr;
}

// iTunes text may be UTF-16BE; everything downstream expects UTF-8.
inline auto utf16BeToUtf8(std::span<const uint8_t> bytes) -> std::string {
  std::string out;
  out.reserve(bytes.size());

  const auto emit = [&out](uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
  };

  for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
    uint32_t cp = (static_cast<uint32_t>(bytes[i]) << 8) | bytes[i + 1];

    if (cp >= 0xD800 && cp <= 0xDBFF) {
      // High surrogate: pair it with the low one, or substitute if it is alone,
      // since a bare surrogate has no valid UTF-8 encoding.
      const uint32_t low = (i + 3 < bytes.size())
                               ? ((static_cast<uint32_t>(bytes[i + 2]) << 8) | bytes[i + 3])
                               : 0u;
      if (low >= 0xDC00 && low <= 0xDFFF) {
        cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
        i += 2;
      } else {
        cp = 0xFFFDu;
      }
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      cp = 0xFFFDu; // low surrogate with nothing before it
    }

    emit(cp);
  }
  return out;
}

// Big-endian integer of whatever width the payload happens to be.
inline auto readBigEndianInt(std::span<const uint8_t> bytes, bool is_signed) -> int64_t {
  const size_t width = std::min<size_t>(bytes.size(), 8);
  if (width == 0) return 0;

  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) {
    value = (value << 8) | bytes[i];
  }
  if (is_signed && width < 8) {
    const uint64_t sign_bit = 1ull << (width * 8 - 1);
    if (value & sign_bit) value |= ~((1ull << (width * 8)) - 1);
  }
  return static_cast<int64_t>(value);
}

// mdhd packs a three-letter ISO 639-2/T code into 16 bits as three 5-bit
// offsets from 0x60. Empty for "und" and for the Macintosh code-page form,
// which sets the top bit and means something else entirely.
inline auto decodeIso639(uint16_t packed) -> std::string {
  if (packed == 0 || (packed & 0x8000u) != 0) return {};

  std::string code(3, '\0');
  for (int i = 0; i < 3; ++i) {
    const auto letter = static_cast<char>(0x60u + ((packed >> (10 - 5 * i)) & 0x1Fu));
    if (letter < 'a' || letter > 'z') return {};
    code[static_cast<size_t>(i)] = letter;
  }
  return code == "und" ? std::string {} : code;
}

// Which sample entry the parser is currently descending into. Codec-config
// boxes such as `esds` appear under both audio and video entries and must be
// interpreted accordingly, so the context travels with the recursion instead
// of living in a pile of per-codec booleans.
enum class SampleEntryKind : uint8_t { None, Mp4aAudio, AlacAudio, Pcm, OtherAudio, Mpeg4Video, OtherVideo };

static auto isAudioEntry(SampleEntryKind kind) -> bool {
  return kind == SampleEntryKind::Mp4aAudio || kind == SampleEntryKind::AlacAudio ||
         kind == SampleEntryKind::Pcm || kind == SampleEntryKind::OtherAudio;
}

// ---------------------------------------------------------------------------
// Parse AudioSpecificConfig (ISO 14496-3)
// Returns true and fills out_sample_rate / out_channels / out_profile.
// ---------------------------------------------------------------------------

// Object types whose config continues with a GASpecificConfig: the plain AAC
// coders and their error-resilient counterparts, which reuse the same layout.
static constexpr auto usesGaSpecificConfig(uint32_t aot) -> bool {
  return (aot >= 1 && aot <= 4) || aot == 6 || aot == 7 || aot == 17 ||
         (aot >= 19 && aot <= 23);
}

static auto parseAudioSpecificConfig(std::span<const uint8_t> asc,
                                     uint32_t& out_sample_rate,
                                     uint32_t& out_channels,
                                     OMProfile& out_profile) -> bool {
  if (asc.empty()) return false;

  BitReader r(asc);

  // Both helpers report 0 rather than a truncated value when the config runs
  // out: BitReader reads past the end as zeroes, and a zero AOT or a zero
  // sampling-frequency index are meaningful values that must not be invented.
  auto readAot = [&]() -> uint32_t {
    if (r.bitsLeft() < 5) return 0;
    const uint32_t aot = r.readBits(5);
    if (aot != 31) return aot;
    return r.bitsLeft() >= 6 ? 32u + r.readBits(6) : 0u;
  };

  // samplingFrequencyIndex (4), with an escape to an explicit 24-bit rate.
  auto readSampleRate = [&]() -> uint32_t {
    if (r.bitsLeft() < 4) return 0;
    const uint32_t idx = r.readBits(4);
    if (idx == 0xF) return r.bitsLeft() >= 24 ? r.readBits(24) : 0u;
    return idx < std::size(AAC_SAMPLE_RATES) ? AAC_SAMPLE_RATES[idx] : 0u;
  };

  uint32_t aot = readAot();
  const uint32_t sample_rate = readSampleRate();

  const uint32_t ch_cfg = r.readBits(4);
  const uint32_t channels = (ch_cfg < std::size(AAC_CHANNELS)) ? AAC_CHANNELS[ch_cfg] : 0u;

  uint32_t ext_sr = 0;
  bool sbr_found = false;
  bool ps_found = false;

  if (aot == 5 || aot == 29) {
    if (aot == 29) ps_found = true;
    ext_sr = readSampleRate();
    aot = readAot();
    sbr_found = true;
  }

  // --- GASpecificConfig — must be consumed before the 0x2B7 extension scan ---
  // frameLengthFlag (1) + dependsOnCoreCoder (1) [+ coreCoderDelay (14)]
  // + extensionFlag (1).
  if (usesGaSpecificConfig(aot)) {
    r.skipBits(1); // frameLengthFlag
    if (r.readFlag()) r.skipBits(14); // dependsOnCoreCoder → coreCoderDelay
    r.skipBits(1); // extensionFlag
  }

  // --- Implicit backwards-compatible SBR/PS signaling via 0x2B7 sync word ---
  //
  // The extension is speculative: a payload that does not start with the sync
  // word, or that turns out not to flag SBR, must leave the cursor untouched
  // so the caller's view of the config stays consistent.
  auto scanExtension = [&](uint32_t want_aot) -> bool {
    const size_t saved = r.bitPosition();
    if (r.bitsLeft() >= 11 && r.readBits(11) == 0x2B7u && readAot() == want_aot &&
        r.readFlag()) {
      return true;
    }
    r.seekBits(saved);
    return false;
  };

  if (!sbr_found && scanExtension(5)) {
    sbr_found = true;
    ext_sr = readSampleRate();
    if (scanExtension(29)) ps_found = true;
  }

  // --- Write outputs ---

  if (sample_rate)            out_sample_rate = sample_rate;
  if (sbr_found && ext_sr)   out_sample_rate = ext_sr;
  if (channels)               out_channels    = channels;
  if (ps_found && out_channels == 1) out_channels = 2; // PS always upmixes mono → stereo

  uint32_t final_aot = aot;
  if (ps_found)       final_aot = 29;
  else if (sbr_found) final_aot = 5;
  if (final_aot > 0)  out_profile = static_cast<OMProfile>(final_aot);

  return true;
}

struct STSCEntry {
  uint32_t first_chunk;
  uint32_t samples_per_chunk;
  uint32_t sample_description_index;
};

struct STTSEntry {
  uint32_t sample_count;
  uint32_t sample_delta;
};

struct CTTSEntry {
  uint32_t sample_count;
  int32_t sample_offset;
};

struct ELSTEntry {
  int64_t segment_duration = 0;
  int64_t media_time = 0;
  int32_t media_rate = 1;
};

struct TREXEntry {
  uint32_t track_id = 0;
  uint32_t default_sample_description_index = 0;
  uint32_t default_sample_duration = 0;
  uint32_t default_sample_size = 0;
  uint32_t default_sample_flags = 0;
};

// Entry counts in the sample tables are 32-bit fields straight out of the
// file, so a handful of corrupt bytes can ask for tens of gigabytes. The box
// body is already bounded by its own header: refuse to reserve more entries
// than the remaining bytes could possibly hold.
inline auto clampEntryCount(const ByteReader& r, uint32_t count,
                            size_t bytes_per_entry) -> uint32_t {
  const size_t max_entries = r.remaining() / bytes_per_entry;
  return static_cast<uint32_t>(std::min<size_t>(count, max_entries));
}

// Timestamps from different tracks only compare once they are off their own
// timescales. Split rather than multiplied first, so a long file cannot
// overflow on the way.
inline auto toNanoseconds(int64_t value, uint32_t timescale) -> int64_t {
  if (timescale == 0) return value;
  const int64_t scale = static_cast<int64_t>(timescale);
  return (value / scale) * 1'000'000'000LL + ((value % scale) * 1'000'000'000LL) / scale;
}

// Same split-then-recombine shape as toNanoseconds, so a long duration cannot
// overflow on the way between two timescales.
inline auto rescaleTime(int64_t value, uint32_t from, uint32_t to) -> int64_t {
  if (from == 0 || to == 0 || from == to) return value;
  const auto from_scale = static_cast<int64_t>(from);
  const auto to_scale = static_cast<int64_t>(to);
  return (value / from_scale) * to_scale + ((value % from_scale) * to_scale) / from_scale;
}

struct Sample {
  int64_t offset = 0;
  uint32_t size = 0;
  int64_t pts = 0;
  int64_t dts = 0;
  uint32_t duration = 0;
  int32_t stream_index = 0;
  bool is_keyframe = false;
};

struct BMFFTrack {
  uint32_t timescale = 0;
  uint32_t handler = 0;
  int32_t index = -1;
  int64_t start_dts = 0;
  int64_t track_duration = 0;
  Track track = {};
  std::unique_ptr<BitStreamFilter> bsf;

  std::vector<uint32_t> sample_sizes;
  std::vector<int64_t> chunk_offsets;
  std::vector<uint32_t> sync_samples;
  std::vector<STSCEntry> stsc_entries;
  std::vector<STTSEntry> stts_entries;
  std::vector<CTTSEntry> ctts_entries;
  std::vector<ELSTEntry> elst_entries;
  std::vector<Sample> samples;
};

// ---------------------------------------------------------------------------
// Leaf-box parsers - accept a pre-read buffer
// ---------------------------------------------------------------------------

// MovieHeaderBox: creation and modification times, then the timescale. The
// times count seconds from 1904-01-01 UTC rather than the Unix epoch.
inline void parseMvhd(std::span<const uint8_t> body, uint32_t& out_movie_timescale,
                      Dictionary& metadata) {
  constexpr int64_t EPOCH_1904_UNIX = -2'082'844'800LL;

  ByteReader r(body);
  const uint8_t version = r.u8();
  r.skip(3);

  const int64_t creation_time = (version == 1) ? static_cast<int64_t>(r.u64be())
                                               : static_cast<int64_t>(r.u32be());
  r.skip(version == 1 ? 8 : 4); // modification_time
  out_movie_timescale = r.u32be();

  if (creation_time > 0 && r.ok()) {
    metadata.setString(CREATION_TIME,
                       date_time::formatIso8601Utc(creation_time + EPOCH_1904_UNIX));
  }
}

inline void parseTkhd(std::span<const uint8_t> body, BMFFTrack& track) {
  ByteReader r(body);
  const uint8_t version = r.u8();
  r.skip(3);
  r.skip(version == 1 ? 16 : 8);
  track.track.id = static_cast<int32_t>(r.u32be());
}

inline void parseMdhd(std::span<const uint8_t> body, BMFFTrack& track) {
  ByteReader r(body);
  const uint8_t version = r.u8();
  r.skip(3);
  if (version == 1) {
    r.skip(16);
    track.timescale = static_cast<uint32_t>(r.u64be());
    track.track_duration = r.i64be();
  } else {
    r.skip(8);
    track.timescale = r.u32be();
    track.track_duration = static_cast<int64_t>(r.u32be());
  }

  if (const std::string language = decodeIso639(r.u16be()); !language.empty()) {
    track.track.metadata.setString(LANGUAGE, language);
  }
}

inline void parseHdlr(std::span<const uint8_t> body, BMFFTrack& track) {
  ByteReader r(body);
  r.skip(8); // version + flags + pre_defined
  // ATOM() packs four-CCs in native order, so the raw bytes must be loaded the
  // same way; a fixed-endian read only matches on little-endian hosts.
  const auto handler_bytes = r.bytes(4);
  if (handler_bytes.size() < 4) return;
  const uint32_t h = load_u32(handler_bytes.data());
  track.handler = h;
  switch (h) {
    case ATOM('s', 'o', 'u', 'n'): track.track.format.type = OM_MEDIA_AUDIO; break;
    case ATOM('v', 'i', 'd', 'e'): track.track.format.type = OM_MEDIA_VIDEO; break;
    default: track.track.format.type = OM_MEDIA_NONE; break;
  }
}

inline void parseElst(std::span<const uint8_t> body, BMFFTrack& track) {
  ByteReader r(body);
  const uint8_t version = r.u8();
  r.skip(3);
  const uint32_t count = clampEntryCount(r, r.u32be(), version == 1 ? 20 : 12);

  track.elst_entries.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    ELSTEntry e;
    if (version == 1) {
      e.segment_duration = r.i64be();
      e.media_time = r.i64be();
    } else {
      e.segment_duration = static_cast<int64_t>(r.u32be());
      e.media_time = static_cast<int64_t>(r.i32be());
    }
    e.media_rate = r.i32be() >> 16;
    track.elst_entries.push_back(e);
  }
}

// Offset subtracted from every decode timestamp, in the track's own timescale.
//
// Two edit-list features fold into this one number, as they do in FFmpeg. A
// leading empty edit (media_time < 0) holds the track silent for its own
// duration, which delays everything after it; the first playable edit then says
// which media time is to appear at that point. Subtracting
// `media_time - empty_duration` produces both: a positive first timestamp when
// the track is delayed, and the usual zero-based timeline when it is not.
//
// Empty-edit durations are in the movie timescale, everything else in the
// track's, so they have to be rescaled before they can be combined.
inline auto getEditListStartDts(const BMFFTrack& track, uint32_t movie_timescale) -> int64_t {
  int64_t empty_duration = 0; // movie timescale

  for (const ELSTEntry& entry : track.elst_entries) {
    if (entry.media_time < 0) {
      empty_duration += entry.segment_duration;
      continue;
    }
    if (entry.media_rate != 1) continue; // a speed-altering segment, not a start point
    return entry.media_time - rescaleTime(empty_duration, movie_timescale, track.timescale);
  }

  // Only empty edits: the track is delayed and then plays from its own start.
  return -rescaleTime(empty_duration, movie_timescale, track.timescale);
}

inline void applyEditList(BMFFTrack& track, uint32_t movie_timescale) {
  track.start_dts = getEditListStartDts(track, movie_timescale);
}

inline void parseColr(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 4) return;
  ByteReader r(body);
  const uint32_t colour_type = load_u32(r.bytes(4).data());

  uint16_t primaries = 0, transfer = 0, matrix = 0;
  OMColorRange range = OM_COLOR_RANGE_UNSPECIFIED;
  switch (colour_type) {
    case ATOM('n', 'c', 'l', 'x'):
      if (body.size() < 11) return;
      primaries = r.u16be();
      transfer = r.u16be();
      matrix = r.u16be();
      range = (r.u8() & 0x80u) ? OM_COLOR_RANGE_FULL : OM_COLOR_RANGE_LIMITED;
      break;
    case ATOM('n', 'c', 'l', 'c'):
    case ATOM('r', 'I', 'C', 'C'):
    case ATOM('p', 'r', 'o', 'f'):
      if (body.size() < 10) return;
      primaries = r.u16be();
      transfer = r.u16be();
      matrix = r.u16be();
      break;
    default:
      return;
  }

  track.track.format.video.color_primaries = color_codes::primariesFromCode(primaries);
  track.track.format.video.color_space = color_codes::colorSpaceFromMatrix(matrix);
  track.track.format.video.transfer_char = color_codes::transferFromCode(transfer);
  track.track.format.video.color_range = range;
}

// PixelAspectRatioBox: hSpacing(32), vSpacing(32). An absent box leaves the
// ratio at {0,0}, which also means square pixels — a caller that only wants to
// know how to scale can treat an unset ratio and an explicit 1:1 alike.
inline void parsePasp(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 8) return;
  const uint32_t h_spacing = load_u32_be(body.data());
  const uint32_t v_spacing = load_u32_be(body.data() + 4);
  if (h_spacing == 0 || v_spacing == 0) return;

  track.track.format.video.sample_aspect_ratio = {static_cast<int>(h_spacing),
                                                  static_cast<int>(v_spacing)};
}

// FieldHandlingBox: fields(8), detail(8). `detail` only means anything for
// interlaced content, where it says which field leads in coding and in display.
inline void parseFiel(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 1) return;

  auto& field_order = track.track.format.video.field_order;
  const uint8_t fields = body[0];

  if (fields == 1) {
    field_order = OM_FIELD_PROGRESSIVE;
    return;
  }
  if (fields != 2 || body.size() < 2) return;

  switch (body[1]) {
    case 1: field_order = OM_FIELD_TT; break;
    case 6: field_order = OM_FIELD_BB; break;
    case 9: field_order = OM_FIELD_BT; break;
    case 14: field_order = OM_FIELD_TB; break;
    default: field_order = OM_FIELD_UNKNOWN; break;
  }
}

// CleanApertureBox: four rationals giving the visible width and height and the
// offset of the clean aperture's centre from the coded picture's centre. The
// box is centre-relative; cropping downstream wants edges, so convert.
//
// Must run after the width and height are known, which it does: the sample
// entry's fixed fields are read before its child boxes.
inline void parseClap(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 32) return;

  auto& video = track.track.format.video;
  const uint32_t width = video.width;
  const uint32_t height = video.height;
  if (width == 0 || height == 0) return;

  ByteReader r(body);
  const int64_t clean_width_n = r.i32be();
  const int64_t clean_width_d = r.i32be();
  const int64_t clean_height_n = r.i32be();
  const int64_t clean_height_d = r.i32be();
  const int64_t horiz_off_n = r.i32be();
  const int64_t horiz_off_d = r.i32be();
  const int64_t vert_off_n = r.i32be();
  const int64_t vert_off_d = r.i32be();
  if (clean_width_d <= 0 || clean_height_d <= 0 || horiz_off_d <= 0 || vert_off_d <= 0) {
    return;
  }

  const int64_t clean_width = clean_width_n / clean_width_d;
  const int64_t clean_height = clean_height_n / clean_height_d;
  if (clean_width <= 0 || clean_height <= 0 ||
      clean_width > width || clean_height > height) {
    return;
  }

  // Centres are expressed in half-pixels to keep the odd-size case exact.
  const int64_t centre_x2 = static_cast<int64_t>(width) - 1 + 2 * (horiz_off_n / horiz_off_d);
  const int64_t centre_y2 = static_cast<int64_t>(height) - 1 + 2 * (vert_off_n / vert_off_d);
  const int64_t left2 = centre_x2 - (clean_width - 1);
  const int64_t top2 = centre_y2 - (clean_height - 1);
  if (left2 < 0 || top2 < 0 || (left2 % 2) != 0 || (top2 % 2) != 0) return;

  const int64_t left = left2 / 2;
  const int64_t top = top2 / 2;
  if (left + clean_width > width || top + clean_height > height) return;

  video.crop.left = static_cast<uint32_t>(left);
  video.crop.top = static_cast<uint32_t>(top);
  video.crop.right = static_cast<uint32_t>(width - clean_width - left);
  video.crop.bottom = static_cast<uint32_t>(height - clean_height - top);
}

inline void parseMdcv(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 24) return;
  ByteReader r(body);
  auto& md = track.track.format.video.mastering_display;
  for (int i = 0; i < 3; ++i) {
    md.display_primaries[i][0] = r.u16be();
    md.display_primaries[i][1] = r.u16be();
  }
  md.white_point[0] = r.u16be();
  md.white_point[1] = r.u16be();
  md.max_display_mastering_luminance = r.u32be();
  md.min_display_mastering_luminance = r.u32be();
  md.has_value = true;
}

inline void parseClli(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 4) return;
  ByteReader r(body);
  auto& cll = track.track.format.video.content_light_level;
  cll.max_content_light_level = r.u16be();
  cll.max_pic_average_light_level = r.u16be();
  cll.has_value = true;
}

inline void parseBtrt(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 12) return;
  ByteReader r(body);
  r.skip(8);
  if (const uint32_t avg = r.u32be(); avg) {
    track.track.bitrate = avg;
  }
}

inline auto parseAvcc(std::span<const uint8_t> body,
                      BMFFTrack& track) -> bool {
  auto config = parseAvcDecoderConfig(body);
  if (!config) return false;

  track.track.format.profile = config->constrained_baseline
      ? OM_PROFILE_H264_CONSTRAINED_BASELINE
      : static_cast<OMProfile>(config->profile_idc);
  track.track.extradata = config->annexb_extradata;
  track.bsf = std::make_unique<AnnexBFilter>(
      config->nal_length_size, std::move(config->annexb_extradata));

  return true;
}

inline auto parseHvcc(std::span<const uint8_t> body,
                      BMFFTrack& track) -> bool {
  auto config = parseHevcDecoderConfig(body);
  if (!config) return false;

  if (config->profile_idc) {
    track.track.format.profile = static_cast<OMProfile>(config->profile_idc);
  }
  track.track.extradata = config->annexb_extradata;
  track.bsf = std::make_unique<AnnexBFilter>(
      config->nal_length_size, std::move(config->annexb_extradata));

  return true;
}

inline auto parseDolbyVisionConfiguration(std::span<const uint8_t> body,
                                          BMFFTrack& track) -> bool {
  if (body.size() < 4) return false;

  ByteReader r(body);
  const uint8_t major = r.u8();
  const uint8_t minor = r.u8();
  const uint8_t profile_level = r.u8();
  const uint8_t flags = r.u8();
  if (!r.ok()) return false;

  track.track.metadata.setBool(DOLBY_VISION_PRESENT, true);
  track.track.metadata.setBinary(DOLBY_VISION_CONFIG, body);
  track.track.metadata.setInt32(DOLBY_VISION_VERSION_MAJOR, major);
  track.track.metadata.setInt32(DOLBY_VISION_VERSION_MINOR, minor);
  track.track.metadata.setInt32(DOLBY_VISION_PROFILE, profile_level >> 1u);
  track.track.metadata.setInt32(DOLBY_VISION_LEVEL, ((profile_level & 0x01u) << 5u) | (flags >> 3u));
  track.track.metadata.setBool(DOLBY_VISION_RPU_PRESENT, (flags & 0x04u) != 0);
  track.track.metadata.setBool(DOLBY_VISION_EL_PRESENT, (flags & 0x02u) != 0);
  track.track.metadata.setBool(DOLBY_VISION_BL_PRESENT, (flags & 0x01u) != 0);

  if (body.size() >= 5) {
    track.track.metadata.setInt32(DOLBY_VISION_BL_SIGNAL_COMPATIBILITY_ID, body[4] >> 4u);
  }

  return true;
}

inline auto parseVvcc(std::span<const uint8_t> body,
                      BMFFTrack& track) -> bool {
  auto config = parseVvcDecoderConfig(body);
  if (!config) return false;

  if (config->profile_idc) {
    track.track.format.profile = static_cast<OMProfile>(config->profile_idc);
  }
  track.track.extradata = config->annexb_extradata;
  track.bsf = std::make_unique<AnnexBFilter>(
      config->nal_length_size, std::move(config->annexb_extradata));

  return true;
}

// EVCDecoderConfigurationRecord: configurationVersion(8), profile_idc(8),
// level_idc(8), toolset_idc_h(32), toolset_idc_l(32), then a byte packing
// chroma_format_idc(2) + bit_depth_luma_minus8(3) + bit_depth_chroma_minus8(3).
// Luma depth has nowhere to live in MediaFormat's video arm, so it is left out
// rather than written into the audio arm of the same union.
inline void parseEvcc(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 12) return;

  track.track.format.profile = static_cast<OMProfile>(body[1]);
  track.track.format.level = body[2];
  track.track.extradata.assign(body.begin(), body.end());
}

// VPCodecConfigurationBox: FullBox header, then profile(8), level(8),
// bitDepth(4)+chromaSubsampling(3)+videoFullRangeFlag(1), colourPrimaries(8),
// transferCharacteristics(8), matrixCoefficients(8). Extradata keeps everything
// from `profile` on, matching what VP9 consumers expect.
inline void parseVpcc(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 5) return;
  track.track.format.profile = static_cast<OMProfile>(body[4]);
  track.track.extradata.assign(body.begin() + 4, body.end());

  if (body.size() < 10) return;
  auto& video = track.track.format.video;
  video.color_range = (body[6] & 0x01u) ? OM_COLOR_RANGE_FULL : OM_COLOR_RANGE_LIMITED;
  video.color_primaries = color_codes::primariesFromCode(body[7]);
  video.transfer_char = color_codes::transferFromCode(body[8]);
  video.color_space = color_codes::colorSpaceFromMatrix(body[9]);
}

inline void parseAv1c(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 2) return;
  track.track.format.profile = static_cast<OMProfile>((body[1] >> 5) & 0x07);
  track.track.extradata.assign(body.begin(), body.end());
}

// OpusSpecificBox: version(8), OutputChannelCount(8), PreSkip(16),
// InputSampleRate(32), OutputGain(16), ChannelMappingFamily(8). InputSampleRate
// only records what the encoder was fed; an Opus stream always decodes at
// 48 kHz, so that is what the track reports.
inline void parseDops(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 11) return;
  track.track.format.audio.channels = body[1];
  track.track.format.audio.sample_rate = 48000u;
  track.track.extradata.assign(body.begin(), body.end());
}

// AC3SpecificBox: fscod(2), bsid(5), bsmod(3), acmod(3), lfeon(1),
// bit_rate_code(5), reserved(5) — three bytes in all. The enclosing
// AudioSampleEntry habitually claims two channels even for a 5.1 programme, so
// this box is the only trustworthy source of the channel count.
inline void parseDac3(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 3) return;
  BitReader r(body);

  const uint32_t fscod = r.readBits(2);
  r.skipBits(5 + 3); // bsid + bsmod
  const uint32_t acmod = r.readBits(3);
  const uint32_t lfeon = r.readBits(1);
  const uint32_t bit_rate_code = r.readBits(5);
  if (!r.ok()) return;

  if (const uint32_t rate = ac3SampleRate(fscod)) {
    track.track.format.audio.sample_rate = rate;
  }
  if (const uint32_t channels = ac3Channels(acmod)) {
    track.track.format.audio.channels = channels + lfeon;
  }
  if (const uint32_t kbps = ac3BitRateKbps(bit_rate_code)) {
    track.track.bitrate = kbps * 1000u;
  }
}

// EC3SpecificBox: data_rate(13), num_ind_sub(3), then one record per
// independent substream. The first substream carries the main programme; the
// rest are alternatives, so only its channel arrangement is reported.
inline void parseDec3(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 5) return;
  BitReader r(body);

  const uint32_t data_rate = r.readBits(13);
  const uint32_t num_ind_sub = r.readBits(3) + 1u;

  uint32_t sample_rate = 0;
  uint32_t channels = 0;

  for (uint32_t i = 0; i < num_ind_sub && r.ok(); ++i) {
    const uint32_t fscod = r.readBits(2);
    r.skipBits(5 + 1 + 1 + 3); // bsid + reserved + asvc + bsmod
    const uint32_t acmod = r.readBits(3);
    const uint32_t lfeon = r.readBits(1);
    r.skipBits(3); // reserved
    const uint32_t num_dep_sub = r.readBits(4);

    uint32_t chan_loc = 0;
    if (num_dep_sub > 0) {
      chan_loc = r.readBits(9);
    } else {
      r.skipBits(1); // reserved
    }

    if (i == 0 && r.ok()) {
      sample_rate = ac3SampleRate(fscod);
      channels = ac3Channels(acmod) + lfeon + eac3DependentChannels(chan_loc);
    }
  }

  if (sample_rate) track.track.format.audio.sample_rate = sample_rate;
  if (channels) track.track.format.audio.channels = channels;
  if (data_rate) track.track.bitrate = data_rate * 1000u;
}

// DTSSpecificBox: DTSSamplingFrequency(32), maxBitrate(32), avgBitrate(32),
// pcmSampleDepth(8), then packed fields ending in a 16-bit ChannelLayout mask.
// That mask is left alone — decoding it correctly needs the asset tables from
// the extension substream, so the sample entry's channel count stands.
inline void parseDdts(std::span<const uint8_t> body, BMFFTrack& track) {
  if (body.size() < 13) return;

  if (const uint32_t rate = load_u32_be(body.data())) {
    track.track.format.audio.sample_rate = rate;
  }
  if (const uint32_t avg_bitrate = load_u32_be(body.data() + 8)) {
    track.track.bitrate = avg_bitrate;
  }
  if (const uint8_t depth = body[12]) {
    track.track.format.audio.bit_depth = depth;
  }
}

inline void parseDfla(std::span<const uint8_t> body, BMFFTrack& track) {
  // FLACSpecificBox: FullBox header followed by FLAC METADATA_BLOCKs, the first
  // of which must be STREAMINFO. Extradata keeps the bare 34-byte STREAMINFO
  // body, the same shape every other FLAC consumer here expects.
  if (body.size() < 4 + 4 + 34) return;
  const uint8_t block_type = body[4] & 0x7Fu;
  if (block_type != 0 || load_u24_be(body.data() + 5) != 34) return;

  const uint8_t* si = body.data() + 8;
  track.track.extradata.assign(si, si + 34);

  // STREAMINFO packs, after the block/frame size fields, 20 bits of sample
  // rate, 3 bits of channels-1 and 5 bits of bits-per-sample-1.
  const uint32_t packed = load_u32_be(si + 10);
  const uint32_t sample_rate = packed >> 12;
  if (sample_rate) track.track.format.audio.sample_rate = sample_rate;
  track.track.format.audio.channels = ((packed >> 9) & 0x7u) + 1;
  track.track.format.audio.bit_depth = ((packed >> 4) & 0x1Fu) + 1;
}

// `is_audio` decides how the DecoderSpecificInfo payload is read: as an
// AudioSpecificConfig for mp4a, or as opaque extradata for mp4v, which carries
// MPEG-4 Visual configuration there and would otherwise get none at all.
inline void parseEsds(std::span<const uint8_t> body, BMFFTrack& track, bool is_audio) {
  ByteReader r(body);
  r.skip(4); // version + flags

  auto readDescLen = [&]() -> uint32_t {
    uint32_t len = 0;
    for (int i = 0; i < 4; ++i) {
      const uint8_t b = r.u8();
      len = (len << 7) | (b & 0x7Fu);
      if (!(b & 0x80u)) break;
    }
    return len;
  };

  if (r.u8() != 0x03) return;
  const uint32_t es_len = readDescLen();
  const size_t es_end = r.tell() + es_len;
  if (es_end > r.size()) return;

  r.skip(2); // ES_ID
  const uint8_t es_flags = r.u8();
  if (es_flags & 0x80u) { r.skip(2); }           // streamDependenceFlag → dependsOn_ES_ID
  if (es_flags & 0x40u) { r.skip(r.u8()); } // URL_Flag → URL_length + URL_string
  if (es_flags & 0x20u) { r.skip(2); }           // OCRstreamFlag → OCR_ES_Id

  while (r.tell() < es_end) {
    const uint8_t tag = r.u8();
    const uint32_t len = readDescLen();
    const size_t next = r.tell() + len;
    if (next > r.size()) break;

    if (tag == 0x04 && len >= 13) {
      // DecoderConfigDescriptor
      r.skip(1); // objectTypeIndication
      r.skip(1); // streamType (6 bits) + upStream (1 bit) + reserved (1 bit)
      r.skip(3); // bufferSizeDB
      r.skip(4); // maxBitrate
      if (const uint32_t avg = r.u32be(); avg) {
        track.track.bitrate = avg;
      }

      while (r.tell() + 2 <= next) {
        const uint8_t sub_tag = r.u8();
        const uint32_t sub_len = readDescLen();
        if (sub_tag == 0x05 && sub_len > 0) {
          // DecoderSpecificInfo — read raw then strip trailing zeros down to 2-byte minimum
          const auto raw_bytes = r.bytes(std::min<size_t>(sub_len, r.remaining()));
          std::vector<uint8_t> raw(raw_bytes.begin(), raw_bytes.end());
          if (is_audio) {
            // Trailing zero padding confuses AudioSpecificConfig parsers; video
            // extradata is opaque and must be handed over byte for byte.
            while (raw.size() > 2 && raw.back() == 0x00) {
              raw.pop_back();
            }
          }
          track.track.extradata = raw;

          if (is_audio) {
            uint32_t sr = track.track.format.audio.sample_rate;
            uint32_t channels = track.track.format.audio.channels;
            OMProfile prof = track.track.format.profile;
            if (parseAudioSpecificConfig(raw, sr, channels, prof)) {
              track.track.format.audio.sample_rate = sr;
              track.track.format.audio.channels = channels;
              track.track.format.profile = prof;
            }
          }
        } else {
          r.skip(sub_len);
        }
      }
    }
    r.seek(next);
  }
}

inline void parseAlacSpecific(std::span<const uint8_t> body, BMFFTrack& track) {
  // ALACSpecificBox: 4-byte version/flags + 24-byte ALACSpecificConfig
  if (body.size() < 28) return;
  ByteReader r(body);
  r.skip(4); // version + flags
  const uint8_t* c = r.cur();
  track.track.format.audio.bit_depth = c[5];
  track.track.format.audio.channels = c[9];
  track.track.format.audio.sample_rate = load_u32_be(c + 20);
  const auto config = r.bytes(24);
  track.track.extradata.assign(config.begin(), config.end());
}

// `stream_size` bounds the constant-size form, which stores no table of its own
// and so is not limited by the box body: every sample still has to fit in the
// file, which caps the plausible sample count.
inline void parseStsz(std::span<const uint8_t> body, BMFFTrack& track,
                      size_t stream_size) {
  ByteReader r(body);
  r.skip(4);
  const uint32_t global_size = r.u32be();
  const uint32_t raw_count = r.u32be();

  if (global_size == 0) {
    const uint32_t count = clampEntryCount(r, raw_count, 4);
    track.sample_sizes.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      track.sample_sizes[i] = r.u32be();
    }
    return;
  }

  const auto max_samples = static_cast<uint32_t>(
      std::min<size_t>(raw_count, stream_size / global_size));
  track.sample_sizes.assign(max_samples, global_size);
}

inline void parseStz2(std::span<const uint8_t> body, BMFFTrack& track) {
  ByteReader r(body);
  r.skip(4);
  r.skip(3); // reserved
  const uint8_t field_size = r.u8();
  // 4-bit fields pack two samples per byte; anything but 4 or 8 is read as 16.
  const uint32_t count = (field_size == 4)
                             ? static_cast<uint32_t>(
                                   std::min<size_t>(r.u32be(), r.remaining() * 2))
                             : clampEntryCount(r, r.u32be(), field_size == 8 ? 1 : 2);
  track.sample_sizes.resize(count);

  switch (field_size) {
    case 4:
      for (uint32_t i = 0; i < count; i += 2) {
        const uint8_t b = r.u8();
        track.sample_sizes[i] = (b >> 4) & 0xFu;
        if (i + 1 < count) track.sample_sizes[i + 1] = b & 0xFu;
      }
      break;
    case 8:
      for (uint32_t i = 0; i < count; ++i) {
        track.sample_sizes[i] = r.u8();
      }
      break;
    default: // 16
      for (uint32_t i = 0; i < count; ++i) {
        track.sample_sizes[i] = r.u16be();
      }
      break;
  }
}

inline void parseStco(std::span<const uint8_t> body, BMFFTrack& track) {
  ByteReader r(body);
  r.skip(4);
  const uint32_t count = clampEntryCount(r, r.u32be(), 4);
  track.chunk_offsets.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    track.chunk_offsets[i] = static_cast<int64_t>(r.u32be());
  }
}

inline void parseCo64(std::span<const uint8_t> body, BMFFTrack& track) {
  ByteReader r(body);
  r.skip(4);
  const uint32_t count = clampEntryCount(r, r.u32be(), 8);
  track.chunk_offsets.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    track.chunk_offsets[i] = static_cast<int64_t>(r.u64be());
  }
}

inline void parseStsc(std::span<const uint8_t> body, BMFFTrack& track) {
  ByteReader r(body);
  r.skip(4);
  const uint32_t count = clampEntryCount(r, r.u32be(), 12);
  track.stsc_entries.resize(count);
  for (auto& e : track.stsc_entries) {
    e.first_chunk = r.u32be();
    e.samples_per_chunk = r.u32be();
    e.sample_description_index = r.u32be();
  }
}

inline void parseStts(std::span<const uint8_t> body, BMFFTrack& track) {
  ByteReader r(body);
  r.skip(4);
  const uint32_t count = clampEntryCount(r, r.u32be(), 8);
  track.stts_entries.resize(count);
  for (auto& e : track.stts_entries) {
    e.sample_count = r.u32be();
    e.sample_delta = r.u32be();
  }
}

inline void parseCtts(std::span<const uint8_t> body, BMFFTrack& track) {
  ByteReader r(body);
  r.skip(4); // version + flags
  const uint32_t count = clampEntryCount(r, r.u32be(), 8);
  track.ctts_entries.resize(count);
  for (auto& e : track.ctts_entries) {
    e.sample_count = r.u32be();
    e.sample_offset = r.i32be();
  }
}

inline void parseStss(std::span<const uint8_t> body, BMFFTrack& track) {
  ByteReader r(body);
  r.skip(4);
  const uint32_t count = clampEntryCount(r, r.u32be(), 4);
  track.sync_samples.resize(count);
  for (auto& s : track.sync_samples) {
    s = r.u32be();
  }
  std::sort(track.sync_samples.begin(), track.sync_samples.end());
}

inline void parseTrex(std::span<const uint8_t> body, std::vector<TREXEntry>& trex_entries) {
  if (body.size() < 24) return;
  ByteReader r(body);
  r.skip(4);

  TREXEntry entry;
  entry.track_id = r.u32be();
  entry.default_sample_description_index = r.u32be();
  entry.default_sample_duration = r.u32be();
  entry.default_sample_size = r.u32be();
  entry.default_sample_flags = r.u32be();

  const auto it = std::find_if(
      trex_entries.begin(), trex_entries.end(),
      [&](const TREXEntry& existing) { return existing.track_id == entry.track_id; });
  if (it != trex_entries.end()) {
    *it = entry;
  } else {
    trex_entries.push_back(entry);
  }
}

// ---------------------------------------------------------------------------
// Sample-table construction - functional style
//
// Iterator state for STTS / CTTS tables is managed with a small helper
// that advances through run-length encoded entries.
// ---------------------------------------------------------------------------

namespace detail {

// Advances through a run-length encoded table (STTS / CTTS).
// Each call to next() returns the current entry value and steps forward.
template<typename Entry, typename ValueFn>
struct RleIterator {
  const std::vector<Entry>& table;
  ValueFn value_fn; // Entry -> value
  size_t idx = 0;
  uint32_t remaining = 0;

  explicit RleIterator(const std::vector<Entry>& t, ValueFn fn)
      : table(t), value_fn(fn) {
    settle();
  }

  auto next() -> decltype(value_fn(table[0])) {
    using T = decltype(value_fn(table[0]));
    if (idx >= table.size()) return T {};
    const auto val = value_fn(table[idx]);
    --remaining;
    settle();
    return val;
  }

  auto valid() const -> bool { return idx < table.size(); }

private:
  // Park on the next entry that actually covers samples. Entries with a zero
  // sample_count are legal to write and do appear; stepping over them in a
  // loop is what keeps `--remaining` from wrapping around on one.
  void settle() {
    while (idx < table.size() && remaining == 0) {
      remaining = table[idx].sample_count;
      if (remaining == 0) ++idx;
    }
  }
};

} // namespace detail

// A sample that does not lie inside the file is not a sample. Checking it here
// keeps every later stage — the merge, seeking, and above all the per-packet
// allocation in readPacket — working from offsets and sizes that are known to
// be readable, instead of trusting a 32-bit size field at read time.
inline auto sampleFitsInStream(int64_t offset, uint32_t size, size_t stream_size) -> bool {
  return offset >= 0 && static_cast<uint64_t>(offset) + size <= stream_size;
}

inline void buildSampleTable(BMFFTrack& track, size_t stream_size) {
  if (track.sample_sizes.empty() ||
      track.chunk_offsets.empty() ||
      track.stsc_entries.empty() ||
      track.stts_entries.empty()) return;

  const size_t num_chunks = track.chunk_offsets.size();

  std::vector<uint32_t> samples_per_chunk_table(num_chunks);
  {
    const auto& entries = track.stsc_entries;
    for (size_t ei = 0; ei < entries.size(); ++ei) {
      // first_chunk is 1-based; 0 is out of range and would wrap on the way to
      // a 0-based index, so the entry is dropped rather than clamped.
      if (entries[ei].first_chunk == 0) continue;
      const uint32_t first = entries[ei].first_chunk - 1u;
      const uint32_t last = (ei + 1 < entries.size() && entries[ei + 1].first_chunk > 0)
                                ? entries[ei + 1].first_chunk - 1u
                                : static_cast<uint32_t>(num_chunks);
      const uint32_t spc = entries[ei].samples_per_chunk;
      for (uint32_t ci = first; ci < last && ci < num_chunks; ++ci) {
        samples_per_chunk_table[ci] = spc;
      }
    }
  }

  track.samples.clear();
  track.samples.reserve(track.sample_sizes.size());

  auto stts_iter = detail::RleIterator(track.stts_entries,
                                       [](const STTSEntry& e) { return e.sample_delta; });
  auto ctts_iter = detail::RleIterator(track.ctts_entries,
                                       [](const CTTSEntry& e) { return e.sample_offset; });

  const bool has_ctts = !track.ctts_entries.empty();

  size_t sample_idx = 0;
  int64_t current_dts = 0;

  for (size_t ci = 0; ci < num_chunks; ++ci) {
    const uint32_t samples_in_chunk = samples_per_chunk_table[ci];
    if (samples_in_chunk == 0) continue;

    int64_t offset = track.chunk_offsets[ci];

    for (uint32_t s = 0;
         s < samples_in_chunk && sample_idx < track.sample_sizes.size();
         ++s) {
      const uint32_t duration = stts_iter.next();
      const int32_t cts_offset = has_ctts ? ctts_iter.next() : 0;

      const int64_t dts = current_dts - track.start_dts;
      const int64_t pts = dts + static_cast<int64_t>(cts_offset);

      const uint32_t sample_number = static_cast<uint32_t>(sample_idx + 1u);
      const bool is_key = track.sync_samples.empty() ||
                          std::binary_search(track.sync_samples.begin(),
                                             track.sync_samples.end(),
                                             sample_number);
      const uint32_t sz = track.sample_sizes[sample_idx];
      if (sampleFitsInStream(offset, sz, stream_size)) {
        track.samples.push_back({offset, sz, pts, dts, duration, 0, is_key});
      }

      offset += sz;
      current_dts += duration;
      ++sample_idx;
    }
  }
}

class BMFFDemuxer final : public BaseDemuxer {
  struct FragmentContext {
    uint32_t track_id = 0;
    uint64_t base_data_offset = 0;
    uint32_t default_sample_duration = 0;
    uint32_t default_sample_size = 0;
    uint32_t default_sample_flags = 0;
    int64_t decode_time = 0;
    bool has_decode_time = false;
  };

  SampleEntryKind current_entry_kind_ = SampleEntryKind::None;
  bool pcm_little_endian_ = false;
  bool in_udta_ = false;
  bool in_ilst_ = false;
  uint32_t movie_timescale_ = 0;

  // Set when the file is structurally broken or unreadable. Parsing stops, but
  // whatever was decoded before that point is still offered to the caller: a
  // file truncated mid-mdat usually has a complete moov and plays fine.
  bool fatal_ = false;

  std::vector<BMFFTrack> bmff_tracks_;
  std::vector<TREXEntry> trex_entries_;
  size_t current_track_slot_ = NO_TRACK;
  std::vector<Sample> samples_;

  // Public track index (the one in `tracks_` and in Sample::stream_index) to
  // its slot in `bmff_tracks_`. The two only coincide while no track is
  // dropped, and timecode or subtitle tracks are dropped all the time.
  std::vector<size_t> public_to_slot_;

  // Positions in `samples_` of each public track's sync samples, ascending.
  // Seeking is a binary search over these rather than a walk back through
  // every sample of every other track.
  std::vector<std::vector<size_t>> keyframes_;

  size_t current_sample_index_ = 0;
  uint64_t current_moof_offset_ = 0;
  std::optional<FragmentContext> current_fragment_;

  RandomRead random_;

  static constexpr size_t NO_TRACK = static_cast<size_t>(-1);

  // The track whose boxes are currently being parsed. A raw pointer would not
  // survive `bmff_tracks_` growing, which a nested or malformed `trak` can
  // cause at any point during parsing.
  auto currentTrack() -> BMFFTrack* {
    return current_track_slot_ < bmff_tracks_.size() ? &bmff_tracks_[current_track_slot_]
                                                     : nullptr;
  }

  auto trackForPublicIndex(int32_t index) -> BMFFTrack* {
    if (index < 0 || static_cast<size_t>(index) >= public_to_slot_.size()) return nullptr;
    return &bmff_tracks_[public_to_slot_[index]];
  }

public:
  BMFFDemuxer() = default;

  // BaseDemuxer::close() only knows about the stream and the public track list;
  // everything parsed out of the container has to be dropped here or a second
  // open() on the same demuxer would parse on top of the first one's state.
  void close() override {
    BaseDemuxer::close();
    bmff_tracks_.clear();
    trex_entries_.clear();
    public_to_slot_.clear();
    keyframes_.clear();
    samples_.clear();
    current_fragment_.reset();
    current_track_slot_ = NO_TRACK;
    current_entry_kind_ = SampleEntryKind::None;
    in_udta_ = false;
    in_ilst_ = false;
    current_sample_index_ = 0;
    current_moof_offset_ = 0;
    movie_timescale_ = 0;
    pcm_little_endian_ = false;
    fatal_ = false;
    parsing_state_ = ParsingState::READING_HEADER;
  }

  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    close();

    input_ = std::move(input);
    if (!input_ || !input_->isValid()) return OM_IO_INVALID_STREAM;
    if (!input_->canSeek()) return OM_IO_SEEK_REQUIRED;

    random_ = RandomRead(input_.get());
    const size_t stream_size = random_.size();

    parseBoxes(0, stream_size);

    for (auto& t : bmff_tracks_) {
      applyEditList(t, movie_timescale_);
      if (t.samples.empty()) {
        buildSampleTable(t, stream_size);
      }
    }

    publishTracks();
    // `fatal_` alone is not a verdict: a file truncated after a complete moov
    // still yields every track it described. Only the absence of usable tracks
    // is, and a structural failure explains why there are none.
    if (tracks_.empty()) {
      return fatal_ ? OM_FORMAT_CORRUPTED : OM_FORMAT_NO_STREAMS;
    }

    mergeSamplesByDecodeTime();
    if (samples_.empty()) return OM_FORMAT_NO_STREAMS;

    buildKeyframeIndex();

    current_sample_index_ = 0;
    parsing_state_ = ParsingState::READY;
    return OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    if (parsing_state_ != ParsingState::READY)
      return Err(OM_COMMON_NOT_INITIALIZED);
    if (current_sample_index_ >= samples_.size())
      return Err(OM_FORMAT_END_OF_FILE);

    const Sample& sample = samples_[current_sample_index_];
    BMFFTrack* bmff_track = trackForPublicIndex(sample.stream_index);
    if (!bmff_track) return Err(OM_FORMAT_STREAM_NOT_FOUND);

    const BitStreamFilter* bsf = bmff_track->bsf.get();
    const auto prefix = (bsf && sample.is_keyframe) ? bsf->keyframePrefix()
                                                    : std::span<const uint8_t> {};

    // The sample is read straight into the packet's own buffer, behind space
    // reserved for the keyframe prefix, and converted there. Staging it in a
    // scratch vector first would mean zero-filling it, reading it, filtering it
    // into a second vector and copying that into the packet — four passes over
    // a buffer that can be several megabytes on a 4K keyframe.
    Packet pkt;
    pkt.allocate(prefix.size() + sample.size);
    if (!random_.read(sample.offset, pkt.bytes.data() + prefix.size(), sample.size))
      return Err(OM_FORMAT_END_OF_FILE);

    if (bsf) {
      const auto payload = pkt.bytes.subspan(prefix.size());
      if (const auto filtered_size = bsf->filterInPlace(payload)) {
        pkt.bytes = pkt.bytes.first(prefix.size() + *filtered_size);
      } else if (!reframeOutOfPlace(pkt, *bsf, prefix.size())) {
        return Err(OM_FORMAT_INVALID_PACKET);
      }
    }

    if (!prefix.empty()) {
      memcpy(pkt.bytes.data(), prefix.data(), prefix.size());
    }

    pkt.stream_index = sample.stream_index;
    pkt.pts = sample.pts;
    pkt.dts = sample.dts;
    pkt.pos = sample.offset;
    pkt.duration = sample.duration;
    pkt.is_keyframe = sample.is_keyframe;

    ++current_sample_index_;
    return Ok(std::move(pkt));
  }

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    if (parsing_state_ != ParsingState::READY || samples_.empty()) {
      return OM_COMMON_NOT_INITIALIZED;
    }
    if (timestamp < 0) {
      return OM_COMMON_INVALID_ARGUMENT;
    }
    if (stream_idx >= static_cast<int32_t>(tracks_.size())) {
      return OM_FORMAT_STREAM_NOT_FOUND;
    }

    // stream_idx < 0 asks in microseconds against the presentation timeline;
    // otherwise the timestamp is in that track's own time base.
    int64_t target_ns;
    if (stream_idx < 0) {
      target_ns = timestamp * INT64_C(1'000);
    } else {
      target_ns = toNanoseconds(timestamp, trackTimescale(stream_idx));
    }

    const size_t best = (mode == SeekMode::DONT_SYNC)
                            ? findSample(target_ns, stream_idx)
                            : findKeyframe(target_ns, stream_idx, mode);
    if (best >= samples_.size()) return OM_FORMAT_STREAM_NOT_FOUND;

    current_sample_index_ = best;
    return OM_SUCCESS;
  }

private:
  auto trackTimescale(int32_t public_index) -> uint32_t {
    const BMFFTrack* track = trackForPublicIndex(public_index);
    const uint32_t timescale = track ? track->timescale : 0;
    return timescale ? timescale : movie_timescale_;
  }

  auto sampleNs(const Sample& sample) -> int64_t {
    return toNanoseconds(sample.dts, trackTimescale(sample.stream_index));
  }

  // The merged list is sorted by decode time in nanoseconds, so both pivots are
  // binary searches. `atOrAfter` is where a forward search starts; `after` is
  // one past the last sample at or before the target, which is where a backward
  // search starts — keeping them apart is what makes a sample landing exactly
  // on the target count as the preceding one rather than the following one.
  struct Pivots {
    size_t at_or_after = 0;
    size_t after = 0;
  };

  auto pivotsFor(int64_t target_ns) -> Pivots {
    const auto lo = std::lower_bound(
        samples_.begin(), samples_.end(), target_ns,
        [&](const Sample& sample, int64_t ns) { return sampleNs(sample) < ns; });
    const auto hi = std::upper_bound(
        lo, samples_.end(), target_ns,
        [&](int64_t ns, const Sample& sample) { return ns < sampleNs(sample); });
    return {static_cast<size_t>(lo - samples_.begin()),
            static_cast<size_t>(hi - samples_.begin())};
  }

  // Unsynced seek: the nearest sample by decode time, restricted to one track
  // when asked. Scans outwards from the pivot, which is a bounded walk because
  // the merged list interleaves the tracks.
  auto findSample(int64_t target_ns, int32_t stream_idx) -> size_t {
    const Pivots pivots = pivotsFor(target_ns);
    const auto belongs = [&](size_t i) {
      return stream_idx < 0 || samples_[i].stream_index == stream_idx;
    };

    size_t prev = samples_.size();
    for (size_t i = pivots.after; i-- > 0;) {
      if (belongs(i)) { prev = i; break; }
    }
    size_t next = samples_.size();
    for (size_t i = pivots.at_or_after; i < samples_.size(); ++i) {
      if (belongs(i)) { next = i; break; }
    }
    return pickNearest(prev, next, target_ns);
  }

  // Synced seek. Keyframes are indexed per track and in ascending order, so the
  // candidates on either side of the target are two binary searches rather than
  // a walk back through every sample of every other track.
  auto findKeyframe(int64_t target_ns, int32_t stream_idx, SeekMode mode) -> size_t {
    const Pivots pivots = pivotsFor(target_ns);

    // The latest keyframe at or before the target, and the earliest at or after
    // it, across the tracks in scope.
    size_t prev = samples_.size();
    size_t next = samples_.size();

    const size_t first_track = stream_idx < 0 ? 0 : static_cast<size_t>(stream_idx);
    const size_t last_track = stream_idx < 0 ? keyframes_.size() : first_track + 1;

    for (size_t track = first_track; track < last_track; ++track) {
      const auto& positions = keyframes_[track];

      const auto before = std::lower_bound(positions.begin(), positions.end(), pivots.after);
      if (before != positions.begin()) {
        const size_t candidate = *(before - 1);
        if (prev == samples_.size() || candidate > prev) prev = candidate;
      }

      const auto at_or_after =
          std::lower_bound(positions.begin(), positions.end(), pivots.at_or_after);
      if (at_or_after != positions.end()) {
        const size_t candidate = *at_or_after;
        if (candidate < next) next = candidate;
      }
    }

    switch (mode) {
      case SeekMode::PREVIOUS_SYNC:
        // Falling back to `next` keeps a seek that lands before the first
        // keyframe from failing outright.
        return prev < samples_.size() ? prev : next;
      case SeekMode::NEXT_SYNC:
        return next < samples_.size() ? next : prev;
      case SeekMode::CLOSEST_SYNC:
      default:
        return pickNearest(prev, next, target_ns);
    }
  }

  auto pickNearest(size_t prev, size_t next, int64_t target_ns) -> size_t {
    if (prev >= samples_.size()) return next;
    if (next >= samples_.size()) return prev;
    const int64_t prev_delta = target_ns - sampleNs(samples_[prev]);
    const int64_t next_delta = sampleNs(samples_[next]) - target_ns;
    return prev_delta <= next_delta ? prev : next;
  }

  // Assign public indices to the tracks worth exposing, and stamp every sample
  // with the index its packets will carry. Tracks that are dropped here — no
  // media type, or no samples — must also drop their samples, or the merge
  // below would emit packets attributed to a track the caller never saw.
  //
  // A recognised media type with no codec id is still published: the caller
  // needs to know the track is there before it can report it as unsupported.
  void publishTracks() {
    public_to_slot_.clear();

    for (size_t slot = 0; slot < bmff_tracks_.size(); ++slot) {
      auto& t = bmff_tracks_[slot];

      if (t.track.format.type == OM_MEDIA_NONE || t.samples.empty()) {
        t.index = -1;
        t.samples.clear();
        t.samples.shrink_to_fit();
        continue;
      }

      t.index = static_cast<int32_t>(public_to_slot_.size());
      t.track.index = t.index;
      public_to_slot_.push_back(slot);

      int64_t min_pts = INT64_MAX;
      int64_t max_pts_end = INT64_MIN;

      for (auto& sample : t.samples) {
        sample.stream_index = t.index;
        min_pts = std::min(min_pts, sample.pts);
        max_pts_end = std::max(max_pts_end, sample.pts + static_cast<int64_t>(sample.duration));
      }

      t.track.start_time = min_pts;
      t.track.duration = (max_pts_end > min_pts) ? (max_pts_end - min_pts) : t.track_duration;
      t.track.nb_frames = static_cast<int64_t>(t.samples.size());
      tracks_.push_back(t.track);
    }
  }

  // Merge the per-track sample lists in decode order.
  //
  // Merging by file offset instead just replays however the file happens to
  // be laid out, and a file may not be interleaved at all: this one stores
  // 1500 video samples before its first audio sample. A reader that follows
  // that order hands a player a minute and a half of video before any sound,
  // which no amount of queueing downstream can absorb. Ordering by timestamp
  // costs nothing here — the samples are addressed by offset anyway — and
  // gives the same interleaving whatever the file looks like.
  void mergeSamplesByDecodeTime() {
    using Iter = std::vector<Sample>::iterator;
    struct Run {
      Iter cur, end;
      uint32_t timescale = 0;
    };

    std::vector<Run> runs;
    runs.reserve(public_to_slot_.size());
    size_t total_samples = 0;

    for (const size_t slot : public_to_slot_) {
      auto& t = bmff_tracks_[slot];
      total_samples += t.samples.size();
      runs.push_back({t.samples.begin(), t.samples.end(),
                      t.timescale ? t.timescale : movie_timescale_});
    }
    samples_.reserve(total_samples);

    while (!runs.empty()) {
      auto best = runs.begin();
      int64_t best_dts = toNanoseconds(best->cur->dts, best->timescale);
      for (auto it = runs.begin() + 1; it != runs.end(); ++it) {
        const int64_t dts = toNanoseconds(it->cur->dts, it->timescale);
        // Equal timestamps keep the file's own order, so a sample is never
        // read further ahead of its neighbours than it has to be.
        if (dts < best_dts || (dts == best_dts && it->cur->offset < best->cur->offset)) {
          best = it;
          best_dts = dts;
        }
      }
      samples_.push_back(*best->cur);
      if (++best->cur == best->end) runs.erase(best);
    }

    // The merged list is the only one read from here on; the per-track copies
    // would otherwise double the demuxer's resident size for the whole session.
    for (const size_t slot : public_to_slot_) {
      bmff_tracks_[slot].samples.clear();
      bmff_tracks_[slot].samples.shrink_to_fit();
    }
  }

  // Fallback for filters that cannot rewrite a payload where it lies, which
  // for Annex-B means NAL length prefixes narrower than the 4-byte start codes
  // replacing them. Reallocates `pkt` around the converted payload, preserving
  // the reserved prefix space. False when the payload could not be converted.
  static auto reframeOutOfPlace(Packet& pkt, const BitStreamFilter& bsf,
                                size_t prefix_size) -> bool {
    const std::span<const uint8_t> payload = pkt.bytes.subspan(prefix_size);
    const auto filtered = bsf.filter(payload);
    if (filtered.bytes.empty()) return false;

    Packet reframed;
    reframed.allocate(prefix_size + filtered.bytes.size());
    memcpy(reframed.bytes.data() + prefix_size, filtered.bytes.data(), filtered.bytes.size());

    pkt.buffer = std::move(reframed.buffer);
    pkt.bytes = reframed.bytes;
    return true;
  }

  void buildKeyframeIndex() {
    keyframes_.assign(public_to_slot_.size(), {});
    for (size_t i = 0; i < samples_.size(); ++i) {
      const Sample& sample = samples_[i];
      if (!sample.is_keyframe) continue;
      const auto index = static_cast<size_t>(sample.stream_index);
      if (index < keyframes_.size()) keyframes_[index].push_back(i);
    }
  }

  // READY is set once open() has a usable sample table; every entry point
  // refuses to run before that. Failures are reported through open()'s return
  // value rather than parked in a state nobody reads back.
  enum class ParsingState { READING_HEADER, READY };
  ParsingState parsing_state_ = ParsingState::READING_HEADER;

  // Read a leaf box body into memory. A short read is fatal for parsing, but
  // the caller keeps whatever it had already decoded.
  auto readBoxData(size_t pos, size_t n) -> std::vector<uint8_t> {
    std::vector<uint8_t> buf;
    if (n > random_.size() || pos > random_.size() - n) {
      fatal_ = true;
      return buf;
    }
    buf.resize(n);
    if (!random_.read(pos, buf.data(), n)) {
      fatal_ = true;
      buf.clear();
    }
    return buf;
  }

  void parseBoxes(size_t start, size_t end) {
    if (fatal_) return;

    size_t pos = start;

    while (!fatal_ && pos + 8 <= end) {
      uint8_t hdr[8];
      if (!random_.read(pos, hdr, 8)) {
        fatal_ = true;
        return;
      }

      const uint32_t raw_size = load_u32_be(hdr);
      const uint32_t type = load_u32(hdr + 4);
      size_t body_pos = pos + 8;
      uint64_t body_size = 0;

      if (raw_size == 1) {
        // 64-bit extended size follows immediately after the 4-byte type, so
        // the header is 16 bytes and has to fit inside the container too.
        if (pos + 16 > end) {
          fatal_ = true;
          return;
        }
        uint8_t large[8];
        if (!random_.read(body_pos, large, 8)) {
          fatal_ = true;
          return;
        }
        const uint64_t full_size = load_u64_be(large);
        body_pos = pos + 16;
        body_size = (full_size >= 16) ? full_size - 16u : 0u;
      } else if (raw_size == 0) {
        // Box extends to the end of the enclosing container.
        body_size = end - body_pos;
      } else if (raw_size < 8) {
        // Malformed: header claims size < minimum. Stop parsing this level.
        fatal_ = true;
        return;
      } else {
        body_size = raw_size - 8u;
      }

      // A box may not reach past its container, whatever its header claims.
      // body_pos <= end holds for every branch above, so this cannot wrap.
      const size_t available = end - body_pos;
      const size_t box_end = body_pos + static_cast<size_t>(
                                            std::min<uint64_t>(body_size, available));

      handleBox(type, pos, body_pos, box_end - body_pos);
      // Every branch leaves box_end >= pos + 8, so the walk always advances.
      pos = box_end;
    }
  }

  // Every `trak` is parsed before the first `moof`, so `bmff_tracks_` no longer
  // grows once fragments are being read and this pointer stays valid.
  auto findTrackById(uint32_t track_id) -> BMFFTrack* {
    const auto it = std::find_if(
        bmff_tracks_.begin(), bmff_tracks_.end(),
        [&](const BMFFTrack& track) { return static_cast<uint32_t>(track.track.id) == track_id; });
    return (it != bmff_tracks_.end()) ? &*it : nullptr;
  }

  auto findTrexByTrackId(uint32_t track_id) const -> const TREXEntry* {
    const auto it = std::find_if(
        trex_entries_.begin(), trex_entries_.end(),
        [&](const TREXEntry& entry) { return entry.track_id == track_id; });
    return (it != trex_entries_.end()) ? &*it : nullptr;
  }

  void parseTfhd(std::span<const uint8_t> body) {
    if (!current_fragment_ || body.size() < 8) return;

    ByteReader r(body);
    r.skip(1);
    const uint32_t flags = (static_cast<uint32_t>(r.u8()) << 16) |
                           (static_cast<uint32_t>(r.u8()) << 8) |
                           static_cast<uint32_t>(r.u8());

    auto& fragment = *current_fragment_;
    fragment.track_id = r.u32be();
    fragment.base_data_offset = current_moof_offset_;
    fragment.default_sample_duration = 0;
    fragment.default_sample_size = 0;
    fragment.default_sample_flags = 0;

    if (const auto* trex = findTrexByTrackId(fragment.track_id)) {
      fragment.default_sample_duration = trex->default_sample_duration;
      fragment.default_sample_size = trex->default_sample_size;
      fragment.default_sample_flags = trex->default_sample_flags;
    }

    if (flags & 0x000001u) {
      fragment.base_data_offset = r.u64be();
    }
    if (flags & 0x000002u) {
      r.u32be();
    }
    if (flags & 0x000008u) {
      fragment.default_sample_duration = r.u32be();
    }
    if (flags & 0x000010u) {
      fragment.default_sample_size = r.u32be();
    }
    if (flags & 0x000020u) {
      fragment.default_sample_flags = r.u32be();
    }
    // Without base-data-offset-present the base is the enclosing moof, which is
    // what `fragment` was initialised to — default-base-is-moof (0x020000) only
    // makes that explicit and needs no separate branch.
  }

  void parseTfdt(std::span<const uint8_t> body) {
    if (!current_fragment_ || body.size() < 8) return;

    ByteReader r(body);
    const uint8_t version = r.u8();
    r.skip(3);

    current_fragment_->decode_time = (version == 1)
        ? static_cast<int64_t>(r.u64be())
        : static_cast<int64_t>(r.u32be());
    current_fragment_->has_decode_time = true;
  }

  void parseTrun(std::span<const uint8_t> body) {
    if (!current_fragment_ || body.size() < 8) return;

    BMFFTrack* track = findTrackById(current_fragment_->track_id);
    if (!track) return;

    ByteReader r(body);
    const uint8_t version = r.u8();
    const uint32_t flags = (static_cast<uint32_t>(r.u8()) << 16) |
                           (static_cast<uint32_t>(r.u8()) << 8) |
                           static_cast<uint32_t>(r.u8());
    const uint32_t sample_count = r.u32be();

    int32_t data_offset = 0;
    if (flags & 0x000001u) {
      data_offset = r.i32be();
    }

    uint32_t first_sample_flags = current_fragment_->default_sample_flags;
    if (flags & 0x000004u) {
      first_sample_flags = r.u32be();
    }

    int64_t sample_offset = static_cast<int64_t>(current_fragment_->base_data_offset) +
                            static_cast<int64_t>(data_offset);
    int64_t current_dts = current_fragment_->has_decode_time
        ? (current_fragment_->decode_time - getEditListStartDts(*track, movie_timescale_))
        : (track->samples.empty() ? 0 : (track->samples.back().dts + static_cast<int64_t>(track->samples.back().duration)));

    // sample_count is a 32-bit field from the file; the loop is bounded by the
    // box body instead, so a corrupt count cannot push millions of zero-sized
    // samples into the track.
    const size_t stream_size = random_.size();
    for (uint32_t i = 0; i < sample_count && r.ok(); ++i) {
      const uint32_t duration = (flags & 0x000100u)
          ? r.u32be()
          : current_fragment_->default_sample_duration;
      const uint32_t size = (flags & 0x000200u)
          ? r.u32be()
          : current_fragment_->default_sample_size;

      uint32_t sample_flags = current_fragment_->default_sample_flags;
      if (flags & 0x000400u) {
        sample_flags = r.u32be();
      } else if ((flags & 0x000004u) && i == 0) {
        sample_flags = first_sample_flags;
      }

      const int32_t cts_offset = (flags & 0x000800u)
          ? ((version == 1) ? r.i32be() : static_cast<int32_t>(r.u32be()))
          : 0;

      const bool is_keyframe = track->track.format.type != OM_MEDIA_VIDEO ||
                               (sample_flags & 0x00010000u) == 0;

      if (!r.ok()) break; // the fields above were read past the end of the box

      if (sampleFitsInStream(sample_offset, size, stream_size)) {
        track->samples.push_back({
            sample_offset,
            size,
            current_dts + static_cast<int64_t>(cts_offset),
            current_dts,
            duration,
            0,
            is_keyframe,
        });
      }

      sample_offset += static_cast<int64_t>(size);
      current_dts += duration;
    }
  }

  void handleBox(uint32_t type, size_t box_start, size_t pos, uint64_t size) {
    if (fatal_) return;

    // Inside `ilst` a box type names a tag rather than a structure, so none of
    // the dispatch below applies.
    if (in_ilst_) {
      parseMetadataItem(type, pos, size);
      return;
    }

    // QuickTime writes the same tags straight into `udta`, without the `meta`
    // and `ilst` wrapping, and with a different payload shape.
    if (in_udta_) {
      if (const Key* key = textMetadataKey(type)) {
        parseQuickTimeUdtaText(*key, pos, size);
        return;
      }
    }

    switch (type) {
      case ATOM('m', 'v', 'e', 'x'):
        parseBoxes(pos, pos + size);
        return;
      case ATOM('m', 'o', 'o', 'f'): {
        const uint64_t saved_moof_offset = current_moof_offset_;
        current_moof_offset_ = box_start;
        parseBoxes(pos, pos + size);
        current_moof_offset_ = saved_moof_offset;
        return;
      }
      case ATOM('t', 'r', 'a', 'f'): {
        const auto saved_fragment = current_fragment_;
        current_fragment_.emplace();
        current_fragment_->base_data_offset = current_moof_offset_;
        parseBoxes(pos, pos + size);
        current_fragment_ = saved_fragment;
        return;
      }
      case ATOM('t', 'r', 'a', 'k'): {
        const size_t saved_slot = current_track_slot_;
        current_track_slot_ = bmff_tracks_.size();
        bmff_tracks_.emplace_back();
        parseBoxes(pos, pos + size);
        current_track_slot_ = saved_slot;
        return;
      }
      case ATOM('u', 'd', 't', 'a'): {
        const bool saved = in_udta_;
        in_udta_ = true;
        parseBoxes(pos, pos + size);
        in_udta_ = saved;
        return;
      }
      case ATOM('i', 'l', 's', 't'): {
        const bool saved = in_ilst_;
        in_ilst_ = true;
        parseBoxes(pos, pos + size);
        in_ilst_ = saved;
        return;
      }
      case ATOM('m', 'e', 't', 'a'): {
        // ISO's `meta` is a FullBox; QuickTime's is not, and the two are told
        // apart by what follows. A FullBox starts with four version/flags bytes
        // and only then the first child, so reading a box header right at the
        // start and finding a plausible type there means there was no FullBox
        // header to skip.
        size_t children = pos + 4;
        uint8_t head[8];
        if (size >= 8 && random_.read(pos, head, 8) &&
            load_u32(head + 4) == ATOM('h', 'd', 'l', 'r')) {
          children = pos;
        }
        parseBoxes(children, pos + size);
        return;
      }
      default:
        break;
    }

    if (isContainerBox(type)) {
      parseBoxes(pos, pos + size);
      return;
    }
    if (isIgnoredBox(type)) return;

    // Media data is addressed by absolute offset out of the sample tables;
    // there is nothing to parse and nothing to remember about the box itself.
    if (type == ATOM('m', 'd', 'a', 't')) return;

    // --- Leaf boxes: read payload into a buffer then dispatch ---
    auto body = readBoxData(pos, size);
    if (fatal_) return;

    if (type == ATOM('m', 'v', 'h', 'd')) {
      parseMvhd(body, movie_timescale_, metadata_);
      return;
    }

    if (type == ATOM('t', 'r', 'e', 'x')) {
      parseTrex(body, trex_entries_);
      return;
    }

    if (type == ATOM('t', 'f', 'h', 'd')) {
      parseTfhd(body);
      return;
    }

    if (type == ATOM('t', 'f', 'd', 't')) {
      parseTfdt(body);
      return;
    }

    if (type == ATOM('t', 'r', 'u', 'n')) {
      parseTrun(body);
      return;
    }

    BMFFTrack* current = currentTrack();
    if (!current) return;
    auto& track = *current;

    switch (type) {
      // Track structure
      case ATOM('t', 'k', 'h', 'd'): parseTkhd(body, track); break;
      case ATOM('m', 'd', 'h', 'd'): parseMdhd(body, track); break;
      case ATOM('h', 'd', 'l', 'r'): parseHdlr(body, track); break;
      case ATOM('e', 'l', 's', 't'): parseElst(body, track); break;

      // Sample table
      case ATOM('s', 't', 's', 'z'): parseStsz(body, track, random_.size()); break;
      case ATOM('s', 't', 'z', '2'): parseStz2(body, track); break;
      case ATOM('s', 't', 'c', 'o'): parseStco(body, track); break;
      case ATOM('c', 'o', '6', '4'): parseCo64(body, track); break;
      case ATOM('s', 't', 's', 'c'): parseStsc(body, track); break;
      case ATOM('s', 't', 't', 's'): parseStts(body, track); break;
      case ATOM('c', 't', 't', 's'): parseCtts(body, track); break;
      case ATOM('s', 't', 's', 's'): parseStss(body, track); break;

      // Video metadata
      case ATOM('b', 't', 'r', 't'): parseBtrt(body, track); break;
      case ATOM('c', 'o', 'l', 'r'): parseColr(body, track); break;
      case ATOM('p', 'a', 's', 'p'): parsePasp(body, track); break;
      case ATOM('f', 'i', 'e', 'l'): parseFiel(body, track); break;
      case ATOM('c', 'l', 'a', 'p'): parseClap(body, track); break;
      case ATOM('m', 'd', 'c', 'v'): parseMdcv(body, track); break;
      case ATOM('C', 'L', 'L', 'I'):
      case ATOM('c', 'l', 'l', 'i'): parseClli(body, track); break;

      // Codec configuration boxes
      case ATOM('a', 'v', 'c', 'C'): parseAvcc(body, track); break;
      case ATOM('h', 'v', 'c', 'C'): parseHvcc(body, track); break;
      case ATOM('d', 'v', 'c', 'C'):
      case ATOM('d', 'v', 'v', 'C'):
        parseDolbyVisionConfiguration(body, track);
        break;
      case ATOM('V', 'v', 'c', 'C'):
      case ATOM('v', 'v', 'c', 'C'): parseVvcc(body, track); break;
      case ATOM('e', 'v', 'c', 'C'): parseEvcc(body, track); break;
      case ATOM('v', 'p', 'c', 'C'): parseVpcc(body, track); break;
      case ATOM('a', 'v', '1', 'C'): parseAv1c(body, track); break;
      case ATOM('d', 'O', 'p', 's'): parseDops(body, track); break;
      case ATOM('d', 'a', 'c', '3'): parseDac3(body, track); break;
      case ATOM('d', 'e', 'c', '3'): parseDec3(body, track); break;
      case ATOM('d', 'd', 't', 's'): parseDdts(body, track); break;

      // esds appears under both mp4a and mp4v entries; only the audio form
      // carries an AudioSpecificConfig, so the entry kind decides how the
      // DecoderSpecificInfo is read.
      case ATOM('e', 's', 'd', 's'):
        if (current_entry_kind_ == SampleEntryKind::Mp4aAudio ||
            current_entry_kind_ == SampleEntryKind::Mpeg4Video) {
          parseEsds(body, track, isAudioEntry(current_entry_kind_));
        }
        break;
      case ATOM('a', 'l', 'a', 'c'):
        if (current_entry_kind_ == SampleEntryKind::AlacAudio) {
          parseAlacSpecific(body, track);
        }
        break;
      // Endianness override for QuickTime PCM, inside the `wave` box.
      case ATOM('e', 'n', 'd', 'a'):
        if (current_entry_kind_ == SampleEntryKind::Pcm && body.size() >= 2) {
          pcm_little_endian_ = load_u16_be(body.data()) != 0;
        }
        break;

      case ATOM('d', 'f', 'L', 'a'): parseDfla(body, track); break;

      // Protection scheme info: the samples are encrypted, and this demuxer
      // does not decrypt. Flag it so the caller can say so instead of handing
      // a decoder ciphertext.
      case ATOM('s', 'c', 'h', 'm'):
        if (body.size() >= 8) {
          track.track.metadata.setBool(ENCRYPTED, true);
          char scheme[5] = {static_cast<char>(body[4]), static_cast<char>(body[5]),
                            static_cast<char>(body[6]), static_cast<char>(body[7]), 0};
          track.track.metadata.setString(ENCRYPTION_SCHEME, std::string_view(scheme));
        }
        break;
      // The original, now-protected sample format. Without it a `encv`/`enca`
      // entry says nothing about what is inside.
      case ATOM('f', 'r', 'm', 'a'):
        if (body.size() >= 4) {
          track.track.metadata.setBool(ENCRYPTED, true);
        }
        break;

      case ATOM('s', 't', 's', 'd'):
        parseStsd(pos, size);
        break;

      default:
        break; // Unknown leaf box - silently skip.
    }
  }

  // Sample descriptions for one track. Only the first usable entry is applied:
  // later entries would otherwise overwrite the codec configuration of the one
  // the samples were actually written against, and choosing per sample would
  // mean honouring stsc's sample_description_index, which nothing downstream
  // can express yet.
  void parseStsd(size_t pos, uint64_t size) {
    if (fatal_) return;

    BMFFTrack* current = currentTrack();
    if (!current) return;
    auto& st = current->track;

    const uint32_t timescale = current->timescale ? current->timescale : movie_timescale_;
    // A zero denominator would propagate into every timestamp conversion the
    // caller performs, so an unusable timescale is reported as 1/1.
    st.time_base = {1, timescale ? static_cast<int>(timescale) : 1};

    if (size < 8) return;
    const size_t stsd_end = pos + static_cast<size_t>(size);

    // Skip FullBox header (version + flags = 4 bytes), then read entry count.
    uint8_t count_buf[4];
    if (!random_.read(pos + 4, count_buf, 4)) {
      fatal_ = true;
      return;
    }
    // Each entry is at least an 8-byte box header, so the box body caps how
    // many of them the count can plausibly describe.
    const uint32_t count = static_cast<uint32_t>(
        std::min<uint64_t>(load_u32_be(count_buf), (size - 8) / 8));

    size_t entry_pos = pos + 8;
    for (uint32_t i = 0; i < count && !fatal_; ++i) {
      if (entry_pos + 8 > stsd_end) return;

      uint8_t entry_hdr[8];
      if (!random_.read(entry_pos, entry_hdr, 8)) {
        fatal_ = true;
        return;
      }

      const uint32_t entry_size = load_u32_be(entry_hdr);
      const uint32_t fmt = load_u32(entry_hdr + 4);

      // An entry claiming less than its own header, or more than the box it
      // sits in, leaves no way to find the next one.
      if (entry_size < 8 || entry_size > stsd_end - entry_pos) return;

      if (parseSampleEntry(fmt, entry_pos, entry_pos + entry_size, st)) return;

      entry_pos += entry_size;
    }
  }

  // Decodes one sample entry into `st`. Returns true once a codec was
  // recognised, which ends the search.
  auto parseSampleEntry(uint32_t fmt, size_t entry_pos, size_t entry_end, Track& st) -> bool {
    const size_t entry_body = entry_pos + 8;

    // Where the codec-specific child boxes start, past the fixed sample-entry
    // fields: 8-byte box header + 28 for AudioSampleEntry, + 78 for
    // VisualSampleEntry.
    constexpr size_t AUDIO_ENTRY_HEADER = 8 + 28;
    constexpr size_t VISUAL_ENTRY_HEADER = 8 + 78;

    const auto audio = [&](OMCodecId codec, SampleEntryKind kind, bool has_children) -> bool {
      st.format.type = OM_MEDIA_AUDIO;
      st.format.codec_id = codec;
      if (!parseAudioSampleEntry(entry_body, st)) return false;
      if (has_children) {
        parseChildBoxes(entry_pos + audioEntryHeaderSize(entry_pos, AUDIO_ENTRY_HEADER),
                        entry_end, kind);
      }
      return true;
    };

    const auto video = [&](OMCodecId codec, SampleEntryKind kind) -> bool {
      st.format.type = OM_MEDIA_VIDEO;
      st.format.codec_id = codec;
      if (!parseVisualSampleEntry(entry_body, st)) return false;
      parseChildBoxes(entry_pos + VISUAL_ENTRY_HEADER, entry_end, kind);
      return true;
    };

    if (fmt == ATOM('a', 'l', 'a', 'c')) {
      return audio(OM_CODEC_ALAC, SampleEntryKind::AlacAudio, true);
    }
    if (isMp4aVariant(fmt)) {
      return audio(OM_CODEC_AAC, SampleEntryKind::Mp4aAudio, true);
    }
    if (fmt == ATOM('f', 'l', 'a', 'c') || fmt == ATOM('f', 'L', 'a', 'C')) {
      return audio(OM_CODEC_FLAC, SampleEntryKind::OtherAudio, true);
    }
    if (fmt == ATOM('o', 'p', 'u', 's') || fmt == ATOM('O', 'p', 'u', 's')) {
      return audio(OM_CODEC_OPUS, SampleEntryKind::OtherAudio, true);
    }
    // dac3 / dec3 sit inside these entries and hold the real channel count.
    if (fmt == ATOM('a', 'c', '-', '3')) {
      return audio(OM_CODEC_AC3, SampleEntryKind::OtherAudio, true);
    }
    if (fmt == ATOM('e', 'c', '-', '3')) {
      return audio(OM_CODEC_EAC3, SampleEntryKind::OtherAudio, true);
    }
    if (fmt == ATOM('a', 'c', '-', '4')) {
      // dac4 describes AC-4 presentations rather than a fixed channel layout,
      // so the track is identified but not further described here.
      return audio(OM_CODEC_AC4, SampleEntryKind::OtherAudio, true);
    }
    if (isDtsVariant(fmt)) {
      return audio(OM_CODEC_DTS, SampleEntryKind::OtherAudio, true);
    }
    if (isPcmVariant(fmt)) return parsePcmSampleEntry(fmt, entry_pos, entry_end, st);

    // A protected entry describes the real codec only through `sinf/frma`.
    // Re-reading it under the original four-CC gives the caller a fully
    // described track that it can then refuse for want of a decryption layer,
    // rather than an unidentified one.
    if (isEncryptedEntry(fmt)) {
      st.metadata.setBool(ENCRYPTED, true);
      const bool is_audio = fmt == ATOM('e', 'n', 'c', 'a');
      const size_t children =
          entry_pos + (is_audio ? audioEntryHeaderSize(entry_pos, AUDIO_ENTRY_HEADER)
                                : VISUAL_ENTRY_HEADER);
      const uint32_t original = findOriginalFormat(children, entry_end);
      if (original == 0 || isEncryptedEntry(original)) return false;
      return parseSampleEntry(original, entry_pos, entry_end, st);
    }

    if (isAvcVariant(fmt)) {
      if (isDolbyVisionAvcVariant(fmt)) {
        st.metadata.setBool(DOLBY_VISION_PRESENT, true);
        st.metadata.setString(DOLBY_VISION_SAMPLE_ENTRY,
                              std::string_view(fmt == ATOM('d', 'v', 'a', '1') ? "dva1" : "dvav"));
      }
      return video(OM_CODEC_H264, SampleEntryKind::OtherVideo);
    }
    if (isHevcVariant(fmt)) {
      if (isDolbyVisionHevcVariant(fmt)) {
        st.metadata.setBool(DOLBY_VISION_PRESENT, true);
        st.metadata.setString(DOLBY_VISION_SAMPLE_ENTRY,
                              std::string_view(fmt == ATOM('d', 'v', 'h', '1') ? "dvh1" : "dvhe"));
      }
      return video(OM_CODEC_HEVC, SampleEntryKind::OtherVideo);
    }
    if (isVvcVariant(fmt)) return video(OM_CODEC_VVC, SampleEntryKind::OtherVideo);
    if (fmt == ATOM('e', 'v', 'c', '1')) return video(OM_CODEC_EVC, SampleEntryKind::OtherVideo);
    if (fmt == ATOM('v', 'p', '0', '8')) return video(OM_CODEC_VP8, SampleEntryKind::OtherVideo);
    if (fmt == ATOM('v', 'p', '0', '9')) return video(OM_CODEC_VP9, SampleEntryKind::OtherVideo);
    if (fmt == ATOM('a', 'v', '0', '1')) return video(OM_CODEC_AV1, SampleEntryKind::OtherVideo);
    if (fmt == ATOM('m', 'p', '4', 'v') || fmt == ATOM('M', 'P', '4', 'V')) {
      return video(OM_CODEC_MPEG4, SampleEntryKind::Mpeg4Video);
    }
    if (const auto prores = proresProfile(fmt)) {
      st.format.profile = *prores;
      return video(OM_CODEC_PRORES, SampleEntryKind::OtherVideo);
    }

    return false; // unrecognised four-CC: try the next entry
  }

  // Tags found under `moov/udta` describe the file; the same boxes under
  // `moov/trak/udta` describe that one track. Which one is being parsed is
  // already known from whether a `trak` is open.
  auto targetMetadata() -> Dictionary& {
    BMFFTrack* track = currentTrack();
    return track ? track->track.metadata : metadata_;
  }

  // One `ilst` child. The box type is the tag; the value sits in a nested
  // `data` box that states how to read it.
  void parseMetadataItem(uint32_t atom, size_t pos, uint64_t size) {
    // Cover art is the only tag that runs large, and a few megabytes of it is
    // already generous; anything beyond that is not a tag worth believing.
    constexpr uint64_t MAX_ITEM_SIZE = 16u * 1024 * 1024;
    if (size < 8 || size > MAX_ITEM_SIZE) return;

    const auto body = readBoxData(pos, static_cast<size_t>(size));
    if (fatal_ || body.empty()) return;

    ByteReader r(body);
    while (r.remaining() >= 8) {
      const uint32_t box_size = r.u32be();
      const auto type_bytes = r.bytes(4);
      if (type_bytes.size() < 4) return;
      const uint32_t box_type = load_u32(type_bytes.data());

      if (box_size < 8 || box_size - 8 > r.remaining()) return;
      const auto payload = r.bytes(box_size - 8);

      if (box_type == ATOM('d', 'a', 't', 'a')) {
        applyMetadataItem(atom, payload);
      }
    }
  }

  // `data` box body: version(8), well-known type(24), locale(32), then value.
  void applyMetadataItem(uint32_t atom, std::span<const uint8_t> data_box) {
    constexpr size_t DATA_HEADER_SIZE = 8;
    if (data_box.size() < DATA_HEADER_SIZE) return;

    const uint32_t data_type = load_u24_be(data_box.data() + 1);
    const auto value = data_box.subspan(DATA_HEADER_SIZE);
    if (value.empty()) return;

    Dictionary& dict = targetMetadata();

    switch (atom) {
      case ATOM('t', 'r', 'k', 'n'):
        // reserved(16), index(16), total(16), reserved(16)
        setNumberPair(dict, value, TRACK_NUMBER, TRACK_TOTAL);
        return;
      case ATOM('d', 'i', 's', 'k'):
        setNumberPair(dict, value, DISC_NUMBER, DISC_TOTAL);
        return;
      case ATOM('t', 'm', 'p', 'o'):
        dict.setInt32(BPM, static_cast<int32_t>(readBigEndianInt(value, false)));
        return;
      case ATOM('c', 'p', 'i', 'l'):
        dict.setBool(COMPILATION, readBigEndianInt(value, false) != 0);
        return;
      case ATOM('g', 'n', 'r', 'e'): {
        // The numeric form, one past the ID3v1 index. A file may carry both
        // this and a textual `©gen`; the text is the better answer, so it is
        // not overwritten here.
        const int64_t index = readBigEndianInt(value, false);
        if (index <= 0 || dict.contains(GENRE)) return;
        if (const auto name = id3v1GenreName(static_cast<size_t>(index - 1)); !name.empty()) {
          dict.setString(GENRE, name);
        }
        return;
      }
      case ATOM('c', 'o', 'v', 'r'):
        dict.setBinary(COVER_ART, value);
        if (const char* mime = imageMimeForDataType(data_type)) {
          dict.setString(COVER_ART_MIME, std::string_view(mime));
        }
        return;
      default:
        break;
    }

    if (const Key* key = textMetadataKey(atom)) {
      if (auto text = decodeText(data_type, value); !text.empty()) {
        dict.setString(*key, text);
      }
    }
  }

  static auto imageMimeForDataType(uint32_t data_type) -> const char* {
    switch (data_type) {
      case DATA_TYPE_JPEG: return "image/jpeg";
      case DATA_TYPE_PNG: return "image/png";
      case DATA_TYPE_BMP: return "image/bmp";
      default: return nullptr;
    }
  }

  // Text tags are normally UTF-8, occasionally UTF-16BE, and sometimes carry
  // the implicit type, which for a tag that is known to be text means UTF-8.
  static auto decodeText(uint32_t data_type, std::span<const uint8_t> value) -> std::string {
    if (data_type == DATA_TYPE_UTF16BE) return utf16BeToUtf8(value);
    if (data_type != DATA_TYPE_UTF8 && data_type != DATA_TYPE_IMPLICIT) return {};

    std::string text(reinterpret_cast<const char*>(value.data()), value.size());
    // Some writers pad to a fixed width with NULs, which would otherwise end up
    // inside the string.
    text.resize(text.find_last_not_of('\0') + 1);
    return text;
  }

  static void setNumberPair(Dictionary& dict, std::span<const uint8_t> value,
                            const Key& index_key, const Key& total_key) {
    if (value.size() < 4) return;
    if (const uint16_t index = load_u16_be(value.data() + 2)) {
      dict.setInt32(index_key, index);
    }
    if (value.size() >= 6) {
      if (const uint16_t total = load_u16_be(value.data() + 4)) {
        dict.setInt32(total_key, total);
      }
    }
  }

  // QuickTime's own form, directly under `udta`: a 16-bit byte count and a
  // 16-bit language code, then the text itself.
  void parseQuickTimeUdtaText(const Key& key, size_t pos, uint64_t size) {
    constexpr uint64_t MAX_ITEM_SIZE = 64u * 1024;
    if (size < 4 || size > MAX_ITEM_SIZE) return;

    const auto body = readBoxData(pos, static_cast<size_t>(size));
    if (fatal_ || body.size() < 4) return;

    const size_t declared = load_u16_be(body.data());
    const auto text = std::span<const uint8_t>(body).subspan(4);
    const auto used = text.first(std::min(declared, text.size()));
    if (used.empty()) return;

    // Language 0 means a Macintosh encoding rather than UTF-8; treating it as
    // UTF-8 is what every other reader does, and is right for plain ASCII.
    if (auto value = decodeText(DATA_TYPE_UTF8, used); !value.empty()) {
      targetMetadata().setString(key, value);
    }
  }

  // Walk a protected sample entry's child boxes for `sinf/frma`, whose payload
  // is the four-CC the entry would have carried unencrypted. 0 when absent.
  auto findOriginalFormat(size_t start, size_t end) -> uint32_t {
    size_t pos = start;
    while (pos + 8 <= end) {
      uint8_t hdr[8];
      if (!random_.read(pos, hdr, 8)) return 0;

      const uint32_t box_size = load_u32_be(hdr);
      const uint32_t type = load_u32(hdr + 4);
      if (box_size < 8 || box_size > end - pos) return 0;

      if (type == ATOM('f', 'r', 'm', 'a')) {
        uint8_t format[4];
        if (box_size < 12 || !random_.read(pos + 8, format, 4)) return 0;
        return load_u32(format);
      }
      // `sinf` is the only container on the path to `frma`.
      if (type == ATOM('s', 'i', 'n', 'f')) {
        if (const uint32_t found = findOriginalFormat(pos + 8, pos + box_size)) return found;
      }
      pos += box_size;
    }
    return 0;
  }

  // QuickTime sound descriptions come in three versions; v1 and v2 insert extra
  // fields between the fixed part and the codec-specific child boxes.
  auto audioEntryHeaderSize(size_t entry_pos, size_t base) -> size_t {
    uint8_t version_buf[2];
    if (!random_.read(entry_pos + 16, version_buf, 2)) return base;
    switch (load_u16_be(version_buf)) {
      case 1: return base + 16;
      case 2: return base + 36;
      default: return base;
    }
  }

  // PCM four-CCs encode the sample layout, and a `wave/enda` child box may
  // override the endianness the four-CC implies.
  auto parsePcmSampleEntry(uint32_t fmt, size_t entry_pos, size_t entry_end, Track& st) -> bool {
    st.format.type = OM_MEDIA_AUDIO;
    if (!parseAudioSampleEntry(entry_pos + 8, st)) return false;

    pcm_little_endian_ = pcmDefaultLittleEndian(fmt);
    parseChildBoxes(entry_pos + audioEntryHeaderSize(entry_pos, 8 + 28), entry_end,
                    SampleEntryKind::Pcm);

    const PcmLayout layout = pcmLayoutOf(fmt);
    st.format.codec_id = pcmCodecId(layout, pcm_little_endian_);
    if (const uint32_t depth = pcmBitDepth(layout)) {
      st.format.audio.bit_depth = depth;
    }
    return true;
  }

  static auto proresProfile(uint32_t fmt) -> std::optional<OMProfile> {
    switch (fmt) {
      case ATOM('a', 'p', 'c', 'o'): return OM_PROFILE_PRORES_PROXY;
      case ATOM('a', 'p', 'c', 's'): return OM_PROFILE_PRORES_LT;
      case ATOM('a', 'p', 'c', 'n'): return OM_PROFILE_PRORES_STANDARD;
      case ATOM('a', 'p', 'c', 'h'): return OM_PROFILE_PRORES_HQ;
      case ATOM('a', 'p', '4', 'h'): return OM_PROFILE_PRORES_4444;
      case ATOM('a', 'p', '4', 'x'): return OM_PROFILE_PRORES_XQ;
      default: return std::nullopt;
    }
  }

  // Descend into a sample entry's codec-configuration boxes, remembering which
  // kind of entry they belong to so boxes shared between media types, such as
  // `esds`, are read the right way.
  void parseChildBoxes(size_t start, size_t end, SampleEntryKind kind) {
    if (start >= end) return;
    const SampleEntryKind saved = current_entry_kind_;
    current_entry_kind_ = kind;
    parseBoxes(start, end);
    current_entry_kind_ = saved;
  }

  auto parseAudioSampleEntry(size_t body_start, Track& track) -> bool {
    // AudioSampleEntry layout (from body_start):
    //   6 bytes reserved + 2 bytes data_ref_index  → skip 8
    //   4 bytes reserved + 4 bytes reserved          → skip 8
    //   After those 16 bytes: channel_count (2), sample_size (2),
    //                          compression_id (2), packet_size (2)
    //                          sample_rate (4, 16.16 fixed-point)
    uint8_t buf[12];
    if (!random_.read(body_start + 16, buf, 12)) return false;

    const uint16_t ch = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
    const uint16_t bits = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];
    const uint32_t sr_fp = load_u32_be(buf + 8);

    if (ch) {
      track.format.audio.channels = ch;
    }
    if (bits) {
      track.format.audio.bit_depth = bits;
    }
    if (sr_fp) {
      track.format.audio.sample_rate = sr_fp >> 16;
    }
    return true;
  }

  auto parseVisualSampleEntry(size_t body_start, Track& track) -> bool {
    // VisualSampleEntry layout from body_start:
    //   6 reserved + 2 data_ref_index + 16 pre_defined/reserved = 24 bytes
    //   Then: width (2), height (2)
    uint8_t vis[4];
    if (!random_.read(body_start + 24, vis, 4)) return false;

    auto& video = track.format.video;
    video.width = load_u16_be(vis);
    video.height = load_u16_be(vis + 2);
    video.color_space = OM_COLOR_SPACE_UNKNOWN;
    video.transfer_char = OM_TRANSFER_UNKNOWN;
    video.color_primaries = OM_PRIMARIES_UNKNOWN;
    video.color_range = OM_COLOR_RANGE_UNSPECIFIED;
    // Cleared here rather than left over from a previous sample entry; the
    // boxes that fill them in are children of this one.
    video.field_order = OM_FIELD_UNKNOWN;
    video.sample_aspect_ratio = {};
    video.crop = {};
    return true;
  }
};

} // namespace

const FormatDescriptor FORMAT_BMFF = {
    .container_id = OM_CONTAINER_MP4,
    .name = "bmff",
    .long_name = "ISO Base Media File Format",
    .demuxer_factory = [] { return std::make_unique<BMFFDemuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
