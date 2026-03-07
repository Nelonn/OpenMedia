#pragma once

#include <openmedia/macro.h>
#include <stdint.h>

OM_ENUM(OMError, int32_t) {
  OM_SUCCESS = 0,

  OM_COMMON_UNKNOWN_ERROR = 1000,
  OM_COMMON_INVALID_ARGUMENT = 1001, // Null ptr, out-of-range value, etc.
  OM_COMMON_NOT_INITIALIZED = 1002,  // Object used before init()
  OM_COMMON_NOT_IMPLEMENTED = 1003,  // Stub / future feature
  OM_COMMON_NOT_SUPPORTED = 1004,    // Feature disabled at compile time
  OM_COMMON_OUT_OF_MEMORY = 1005,
  OM_COMMON_OVERFLOW = 1006, // Integer/buffer overflow detected
  OM_COMMON_TIMEOUT = 1007,
  OM_COMMON_CANCELLED = 1008, // Operation aborted by user
  OM_COMMON_PERMISSION_DENIED = 1009,

  OM_IO_UNKNOWN_ERROR = 2000,
  OM_IO_INVALID_STREAM = 2001,  // Stream ptr is null / closed
  OM_IO_SEEK_REQUIRED = 2002,   // Operation needs seekable stream
  OM_IO_SEEK_FAILED = 2003,     // fseek/lseek returned error
  OM_IO_NOT_ENOUGH_DATA = 2004, // Tried to read N bytes, got less
  OM_IO_END_OF_STREAM = 2005,   // Clean EOF
  OM_IO_WRITE_FAILED = 2006,
  OM_IO_READ_FAILED = 2007,
  OM_IO_OPEN_FAILED = 2008,   // File/URL couldn't be opened
  OM_IO_NETWORK_ERROR = 2009, // Socket-level failure
  OM_IO_NETWORK_TIMEOUT = 2010,
  OM_IO_NETWORK_INTERRUPTED = 2011, // Connection dropped mid-stream
  OM_IO_PROTOCOL_ERROR = 2012,      // e.g. malformed HTTP response

  OM_FORMAT_UNKNOWN_ERROR = 3000,
  OM_FORMAT_DETECTION_FAILED = 3001,  // Can't identify container type
  OM_FORMAT_NOT_SUPPORTED = 3002,     // Known format but not implemented
  OM_FORMAT_INVALID_HEADER = 3003,    // Magic bytes ok, header corrupt
  OM_FORMAT_CORRUPTED = 3004,         // Mid-stream structural corruption
  OM_FORMAT_END_OF_FILE = 3005,       // No more packets
  OM_FORMAT_NO_STREAMS = 3006,        // Container has zero A/V streams
  OM_FORMAT_STREAM_NOT_FOUND = 3007,  // Requested stream index missing
  OM_FORMAT_INVALID_TIMESTAMP = 3008, // PTS/DTS out of range or missing
  OM_FORMAT_MUXING_FAILED = 3009,     // Error writing container
  OM_FORMAT_PARSE_FAILED = 3010,      // Packet framing broken
  OM_FORMAT_SYNC_LOST = 3011,         // Lost sync, trying to resync
  OM_FORMAT_INVALID_PACKET = 3012,    // Packet size/flags make no sense
  OM_FORMAT_MISSING_KEYFRAME = 3013,  // Stream started without keyframe

  OM_CODEC_UNKNOWN_ERROR = 4000,
  OM_CODEC_NOT_FOUND = 4001,      // No decoder for codec_id
  OM_CODEC_NOT_SUPPORTED = 4002,  // Profile/level not supported
  OM_CODEC_OPEN_FAILED = 4003,
  OM_CODEC_INVALID_PARAMS = 4004, // Bad width/height/sample_rate/etc.
  OM_CODEC_DECODE_FAILED = 4005,  // Unrecoverable decode error
  OM_CODEC_ENCODE_FAILED = 4006,
  OM_CODEC_NEED_MORE_DATA = 4007, // Send more packets (drain loop)
  OM_CODEC_FLUSHED = 4008,        // Codec is drained, no more frames
  OM_CODEC_HWACCEL_FAILED = 4009, // GPU init or decode failure
  OM_CODEC_HWACCEL_NOT_FOUND = 4010,

  OM_FRAME_INVALID = 5000,          // Null/unallocated frame
  OM_FRAME_WRONG_FORMAT = 5001,     // Pixel/sample fmt mismatch
  OM_FRAME_WRONG_SIZE = 5002,       // Resolution changed unexpectedly
  OM_FRAME_REORDER_OVERFLOW = 5003, // B-frame reorder buffer full
  OM_FRAME_CORRUPT = 5004,          // Marked corrupt by decoder
};
