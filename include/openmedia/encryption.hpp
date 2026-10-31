#pragma once

#include <cstdint>
#include <string>
#include <openmedia/macro.h>
#include <openmedia/media.h>
#include <vector>

// ISO/IEC 23001-7. Only payloads need a key, which is the caller's to obtain; licence init
// data is in the metadata under `ENCRYPTION_INIT_DATA`.
OM_ENUM(OMEncryptionScheme, uint8_t) {
    OM_ENCRYPTION_NONE = 0,

    // AES-128-CTR over whole protected ranges.
    OM_ENCRYPTION_CENC,
    // AES-128-CBC over whole protected ranges.
    OM_ENCRYPTION_CBC1,
    // AES-128-CTR over a crypt/skip pattern within each protected range.
    OM_ENCRYPTION_CENS,
    // AES-128-CBC over a crypt/skip pattern within each protected range.
    OM_ENCRYPTION_CBCS,
};

namespace openmedia {

/**
 * One clear/protected split of a payload, in storage order. A payload's entries
 * cover it exactly, so a decryptor walks them without needing offsets.
 */
struct SubsampleEntry {
  uint32_t clear_bytes = 0;
  uint32_t protected_bytes = 0;
};

/**
 * What it takes to turn one packet's payload into plaintext.
 *
 * Byte counts address the packet as handed out, not as it sat in the container,
 * so a payload the demuxer reframed on the way is already accounted for.
 */
struct OPENMEDIA_ABI SampleEncryption {
  OMEncryptionScheme scheme = OM_ENCRYPTION_NONE;

  // The key the licence has to supply, as a `cenc` KID.
  uint8_t key_id[16] = {};

  // HLS SAMPLE-AES key URI for the segment that produced this packet. A
  // container KID may be zero across a playlist's key rotation, so the
  // playlist URI is the key selector when HLS supplies one.
  std::string key_uri;

  // Left-aligned and zero-padded, so AES-CTR can take it as the initial counter
  // block and AES-CBC as the IV. `iv_size` is what the container carried, 8 or 16.
  uint8_t iv[16] = {};
  uint8_t iv_size = 0;

  // The `cens` and `cbcs` pattern, in 16-byte blocks. Both zero means no pattern,
  // so every protected byte is enciphered.
  uint8_t crypt_byte_block = 0;
  uint8_t skip_byte_block = 0;

  // Empty means the whole payload is protected.
  std::vector<SubsampleEntry> subsamples;

  // Non-zero: NAL units behind prefixes this wide, as stored, rather than Annex-B. Happens where
  // reframing would invalidate the map above, so decipher first and reframe after.
  uint8_t nal_length_size = 0;
};

} // namespace openmedia
