#pragma once

#include <cstdint>
#include <openmedia/macro.h>
#include <openmedia/media.h>
#include <vector>

// Common Encryption, ISO/IEC 23001-7: the cipher covers the media samples and
// nothing else, so a container is parsed as usual and only the payloads need a
// key. What a decryption module -- Widevine, PlayReady, FairPlay -- is handed
// per sample is described here; obtaining the key is the caller's business, and
// the initialization data a licence request starts from is in the track and
// container metadata under `ENCRYPTION_INIT_DATA`.
OM_ENUM(OMEncryptionScheme, uint8_t) {
    OM_ENCRYPTION_NONE = 0,

    // AES-128-CTR over whole protected ranges.
    OM_ENCRYPTION_CENC,
    // AES-128-CBC over whole protected ranges.
    OM_ENCRYPTION_CBC1,
    // AES-128-CTR over a crypt/skip pattern within each protected range.
    OM_ENCRYPTION_CENS,
    // AES-128-CBC over a crypt/skip pattern within each protected range. The
    // scheme adaptive streaming has settled on, together with `cenc`.
    OM_ENCRYPTION_CBCS,
};

namespace openmedia {

/**
 * One clear/protected split of a payload, in storage order. A payload's entries
 * cover it exactly and in order, so a decryptor walks them without needing any
 * offsets: `clear_bytes` to copy, then `protected_bytes` to decipher.
 */
struct SubsampleEntry {
  uint32_t clear_bytes = 0;
  uint32_t protected_bytes = 0;
};

/**
 * What it takes to turn one packet's payload into plaintext: which key, which
 * initialization vector, and which parts of the payload the cipher covers.
 *
 * Every byte count here addresses the packet as it was handed out, not as it sat
 * in the container, so a payload the demuxer reframed on the way is already
 * accounted for.
 */
struct OPENMEDIA_ABI SampleEncryption {
  OMEncryptionScheme scheme = OM_ENCRYPTION_NONE;

  // The key the licence has to supply, as the 16 raw bytes of a `cenc` KID.
  uint8_t key_id[16] = {};

  // Left-aligned and zero-padded to 16 bytes, which for AES-CTR makes it the
  // initial counter block and for AES-CBC the IV as it stands. `iv_size` is how
  // many bytes the container carried, 8 or 16.
  uint8_t iv[16] = {};
  uint8_t iv_size = 0;

  // The pattern of the `cens` and `cbcs` schemes, counted in 16-byte blocks:
  // `crypt_byte_block` enciphered, then `skip_byte_block` left alone, repeating.
  // Both zero means no pattern, and every protected byte is enciphered.
  uint8_t crypt_byte_block = 0;
  uint8_t skip_byte_block = 0;

  // Empty means the whole payload is protected.
  std::vector<SubsampleEntry> subsamples;
};

} // namespace openmedia
