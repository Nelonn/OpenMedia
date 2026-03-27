#pragma once

#include <cstdint>
#include <openmedia/dictionary.hpp>

namespace openmedia {

// int32 - Encoding application mode (VOIP(2048)/AUDIO(2049)/RESTRICTED_LOWDELAY(2051))
constexpr Key OPUS_ENC_APPLICATION = "opus.application";
// int32 - Encoder complexity (0-10, default 10)
constexpr Key OPUS_ENC_COMPLEXITY = "opus.complexity";
// int32 - Encoder lookahead delay in samples
constexpr Key OPUS_ENC_LOOKAHEAD = "opus.lookahead";
// bool - Force low delay mode
constexpr Key OPUS_ENC_LOW_DELAY = "opus.low_delay";
// int32 - Expected packet loss percentage (0-100)
constexpr Key OPUS_ENC_PACKET_LOSS_PERC = "opus.packet_loss_perc";
// bool - Enable inband FEC
constexpr Key OPUS_ENC_FEC = "opus.fec";
// bool - DTX
constexpr Key OPUS_DTX = "opus.dtx";

/**
 * @brief Key type enumeration for libopus encoder options
 * 
 * Each key defines the expected value type for type-safe access.
 */
enum class OpusEncKey : uint8_t {
  // Basic encoding parameters
  APPLICATION,    ///< int32 - Encoding application mode (VOIP/AUDIO/RESTRICTED_LOWDELAY)
  SAMPLE_RATE,    ///< int32 - Input sample rate (8000/12000/16000/24000/48000)
  CHANNELS,       ///< int32 - Number of channels (1-255)

  // Bitrate control
  BITRATE,        ///< int32 - Target bitrate in bps (OPUS_AUTO or specific value)
  MAX_BITRATE,    ///< int32 - Maximum bitrate in bps
  VBR_CONSTRAINT, ///< int32 - VBR constraint (0=disabled, 1=enabled)

  // VBR settings
  VBR,            ///< int32 - VBR mode (0=CBR, 1=VBR, 2=constrained VBR, 3=hybrid VBR)
  VBR_QUALITY,    ///< int32 - VBR quality factor (0-10, default 10)

  // Complexity
  COMPLEXITY,     ///< int32 - Encoder complexity (0-10, default 10)

  // Frame size and timing
  FRAME_SIZE,     ///< int32 - Frame size in samples
  LOOKAHEAD,      ///< int32 - Encoder lookahead delay in samples
  FORCE_CHANNELS, ///< int32 - Force stereo/mono (-1=auto)

  // Signal type
  SIGNAL_TYPE,    ///< int32 - Signal type hint (MUSIC/VOICE)

  // Bandwidth
  BANDWIDTH,      ///< int32 - Audio bandwidth (NARROWBAND to FULLBAND)

  // Low delay and error resilience
  LOW_DELAY,      ///< int32 - Force low delay mode (0 or 1)
  PACKET_LOSS_PERC, ///< int32 - Expected packet loss percentage (0-100)
  FEC,            ///< int32 - Enable inband FEC (0 or 1)

  // DTX
  DTX,            ///< int32 - Enable DTX (0 or 1)

  // Quantization
  LSB_DEPTH,      ///< int32 - LSB depth for quantization (4-24 bits)

  // Stereo processing
  PHASE_INVERSION, ///< int32 - Enable phase inversion (0 or 1)
  MONO_RATIO,     ///< int32 - Mono downmix ratio (0-100, default auto)
};

/**
 * @brief Get the dictionary key for an Opus encoder option
 * @param key The OpusEncKey enum value
 * @return The corresponding Key object for dictionary access
 */
constexpr auto getOpusEncKey(OpusEncKey key) -> Key {
  switch (key) {
    case OpusEncKey::APPLICATION:     return Key("opus.application");
    case OpusEncKey::SAMPLE_RATE:     return Key("opus.sample_rate");
    case OpusEncKey::CHANNELS:        return Key("opus.channels");
    case OpusEncKey::BITRATE:         return Key("opus.bitrate");
    case OpusEncKey::MAX_BITRATE:     return Key("opus.max_bitrate");
    case OpusEncKey::VBR_CONSTRAINT:  return Key("opus.vbr_constraint");
    case OpusEncKey::VBR:             return Key("opus.vbr");
    case OpusEncKey::VBR_QUALITY:     return Key("opus.vbr_quality");
    case OpusEncKey::COMPLEXITY:      return Key("opus.complexity");
    case OpusEncKey::FRAME_SIZE:      return Key("opus.frame_size");
    case OpusEncKey::LOOKAHEAD:       return Key("opus.lookahead");
    case OpusEncKey::FORCE_CHANNELS:  return Key("opus.force_channels");
    case OpusEncKey::SIGNAL_TYPE:     return Key("opus.signal_type");
    case OpusEncKey::BANDWIDTH:       return Key("opus.bandwidth");
    case OpusEncKey::LOW_DELAY:       return Key("opus.low_delay");
    case OpusEncKey::PACKET_LOSS_PERC: return Key("opus.packet_loss_perc");
    case OpusEncKey::FEC:             return Key("opus.fec");
    case OpusEncKey::DTX:             return Key("opus.dtx");
    case OpusEncKey::LSB_DEPTH:       return Key("opus.lsb_depth");
    case OpusEncKey::PHASE_INVERSION: return Key("opus.phase_inversion");
    case OpusEncKey::MONO_RATIO:      return Key("opus.mono_ratio");
    default:                          return Key();
  }
}

/**
 * @brief Helper to set an Opus encoder option in a dictionary
 * @param dict The dictionary to set the option in
 * @param key The OpusEncKey enum value
 * @param value The int32 value to set
 */
inline auto setOpusEncOption(Dictionary& dict, OpusEncKey key, int32_t value) -> void {
  dict.setInt32(getOpusEncKey(key), value);
}

/**
 * @brief Helper to get an Opus encoder option from a dictionary
 * @param dict The dictionary to get the option from
 * @param key The OpusEncKey enum value
 * @param defaultValue Default value if not present
 * @return The int32 value
 */
inline auto getOpusEncOption(const Dictionary& dict, OpusEncKey key, int32_t defaultValue = 0) -> int32_t {
  return dict.getInt32(getOpusEncKey(key), defaultValue);
}

/**
 * @brief Helper to check if an Opus encoder option is set
 * @param dict The dictionary to check
 * @param key The OpusEncKey enum value
 * @return true if the option is present
 */
inline auto hasOpusEncOption(const Dictionary& dict, OpusEncKey key) -> bool {
  return dict.contains(getOpusEncKey(key));
}

} // namespace openmedia
