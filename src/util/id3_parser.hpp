#pragma once

#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <openmedia/dictionary.hpp>
#include <openmedia/metadata_keys.hpp>
#include <openmedia/track.hpp>
#include <span>
#include <string>
#include <string_view>
#include <util/id3_genres.hpp>
#include <util/io_util.hpp>
#include <vector>

namespace openmedia {

inline auto readExact(InputStream& stream, std::span<uint8_t> dst) -> size_t {
  size_t total = 0;
  while (total < dst.size()) {
    size_t got = stream.read(dst.subspan(total));
    if (got == 0) break;
    total += got;
  }
  return total;
}

inline auto latin1ToUtf8(std::span<const uint8_t> bytes) -> std::string {
  std::string out;
  out.reserve(bytes.size());
  for (uint8_t c : bytes) {
    if (c == 0) break;
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back(static_cast<char>(0xC0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

inline auto utf16ToUtf8(std::span<const uint8_t> bytes, bool force_be = false) -> std::string {
  std::string out;
  if (bytes.size() < 2) return out;
  bool little_endian = !force_be;
  size_t i = 0;
  if (!force_be && bytes.size() >= 2) {
    if (bytes[0] == 0xFF && bytes[1] == 0xFE) {
      little_endian = true;
      i = 2;
    } else if (bytes[0] == 0xFE && bytes[1] == 0xFF) {
      little_endian = false;
      i = 2;
    }
  }
  auto read_u16 = [&](size_t offset) -> uint16_t {
    if (little_endian) {
      return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
    } else {
      return static_cast<uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
    }
  };
  while (i + 1 < bytes.size()) {
    uint16_t code = read_u16(i);
    i += 2;
    if (code == 0) break;
    uint32_t cp = code;
    if (code >= 0xD800 && code <= 0xDBFF && i + 1 < bytes.size()) {
      uint16_t low = read_u16(i);
      if (low >= 0xDC00 && low <= 0xDFFF) {
        cp = 0x10000 + ((static_cast<uint32_t>(code) - 0xD800) << 10) + (static_cast<uint32_t>(low) - 0xDC00);
        i += 2;
      }
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

inline auto decodeId3Text(uint8_t encoding, std::span<const uint8_t> data) -> std::string {
  if (data.empty()) return {};
  if (encoding == 0) {
    return latin1ToUtf8(data);
  } else if (encoding == 1) {
    return utf16ToUtf8(data, false);
  } else if (encoding == 2) {
    return utf16ToUtf8(data, true);
  } else if (encoding == 3) {
    size_t len = data.size();
    while (len > 0 && data[len - 1] == 0) {
      --len;
    }
    return std::string(reinterpret_cast<const char*>(data.data()), len);
  }
  return latin1ToUtf8(data);
}

inline auto findNullTerminator(uint8_t encoding, std::span<const uint8_t> data, size_t start = 0) -> size_t {
  if (encoding == 1 || encoding == 2) {
    for (size_t i = start; i + 1 < data.size(); i += 2) {
      if (data[i] == 0 && data[i + 1] == 0) {
        return i;
      }
    }
    return data.size();
  }
  for (size_t i = start; i < data.size(); ++i) {
    if (data[i] == 0) {
      return i;
    }
  }
  return data.size();
}

inline auto nullTerminatorLength(uint8_t encoding) -> size_t {
  return (encoding == 1 || encoding == 2) ? 2 : 1;
}

inline auto resolveId3Genre(std::string_view text) -> std::string {
  if (text.empty()) return {};
  if (text.front() == '(') {
    size_t close_paren = text.find(')');
    if (close_paren != std::string_view::npos) {
      std::string_view num_str = text.substr(1, close_paren - 1);
      int idx = 0;
      auto [p, ec] = std::from_chars(num_str.data(), num_str.data() + num_str.size(), idx);
      if (ec == std::errc {} && idx >= 0) {
        auto name = id3v1GenreName(static_cast<size_t>(idx));
        std::string_view remainder = text.substr(close_paren + 1);
        if (!remainder.empty()) {
          return std::string(remainder);
        }
        if (!name.empty()) {
          return std::string(name);
        }
      }
    }
  }
  bool all_digits = true;
  for (char c : text) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      all_digits = false;
      break;
    }
  }
  if (all_digits) {
    int idx = 0;
    auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), idx);
    if (ec == std::errc {} && idx >= 0) {
      auto name = id3v1GenreName(static_cast<size_t>(idx));
      if (!name.empty()) {
        return std::string(name);
      }
    }
  }
  return std::string(text);
}

inline void parseNumberPair(std::string_view text, const Key& num_key, const Key& total_key, Dictionary& dict) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  if (text.empty()) return;
  int num = 0;
  auto [p1, ec1] = std::from_chars(text.data(), text.data() + text.size(), num);
  if (ec1 == std::errc {} && num > 0) {
    dict.setInt32(num_key, num);
    size_t offset = static_cast<size_t>(p1 - text.data());
    if (offset < text.size() && text[offset] == '/') {
      std::string_view rest = text.substr(offset + 1);
      while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) {
        rest.remove_prefix(1);
      }
      int total = 0;
      auto [p2, ec2] = std::from_chars(rest.data(), rest.data() + rest.size(), total);
      if (ec2 == std::errc {} && total > 0) {
        dict.setInt32(total_key, total);
      }
    }
  }
}

inline auto deunsynchronize(std::span<const uint8_t> in) -> std::vector<uint8_t> {
  std::vector<uint8_t> out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    out.push_back(in[i]);
    if (in[i] == 0xFF && i + 1 < in.size() && in[i + 1] == 0x00) {
      ++i;
    }
  }
  return out;
}

inline auto iequals(std::string_view a, std::string_view b) noexcept -> bool {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

inline void parseId3v2(std::span<const uint8_t> tag_data, Dictionary& metadata) {
  if (tag_data.size() < 10) return;
  if (std::memcmp(tag_data.data(), "ID3", 3) != 0) return;

  uint8_t version_major = tag_data[3];
  if (version_major < 2 || version_major > 4) return;

  uint8_t flags = tag_data[5];
  bool tag_unsync = (flags & 0x80) != 0;
  bool extended_hdr = (flags & 0x40) != 0;

  for (size_t i = 6; i < 10; ++i) {
    if (tag_data[i] & 0x80) return;
  }
  uint32_t payload_size = (static_cast<uint32_t>(tag_data[6]) << 21) |
                          (static_cast<uint32_t>(tag_data[7]) << 14) |
                          (static_cast<uint32_t>(tag_data[8]) << 7) |
                          static_cast<uint32_t>(tag_data[9]);

  size_t total_size = 10 + payload_size;
  if (total_size > tag_data.size()) {
    payload_size = static_cast<uint32_t>(tag_data.size() - 10);
  }

  std::span<const uint8_t> payload = tag_data.subspan(10, payload_size);
  std::vector<uint8_t> deunsynced_payload;
  if (tag_unsync && version_major < 4) {
    deunsynced_payload = deunsynchronize(payload);
    payload = deunsynced_payload;
  }

  size_t cursor = 0;
  if (extended_hdr) {
    if (version_major == 3 && cursor + 4 <= payload.size()) {
      uint32_t ext_size = load_u32_be(payload.data() + cursor);
      cursor += 4 + ext_size;
    } else if (version_major == 4 && cursor + 4 <= payload.size()) {
      uint32_t ext_size = (static_cast<uint32_t>(payload[cursor]) << 21) |
                          (static_cast<uint32_t>(payload[cursor + 1]) << 14) |
                          (static_cast<uint32_t>(payload[cursor + 2]) << 7) |
                          static_cast<uint32_t>(payload[cursor + 3]);
      cursor += ext_size;
    }
  }

  while (cursor < payload.size()) {
    if (payload[cursor] == 0) break;

    std::string_view frame_id;
    uint32_t frame_size = 0;
    size_t header_len = 0;
    bool frame_unsync = false;
    bool data_length_indicator = false;

    if (version_major == 2) {
      if (cursor + 6 > payload.size()) break;
      frame_id = std::string_view(reinterpret_cast<const char*>(payload.data() + cursor), 3);
      frame_size = load_u24_be(payload.data() + cursor + 3);
      header_len = 6;
    } else {
      if (cursor + 10 > payload.size()) break;
      frame_id = std::string_view(reinterpret_cast<const char*>(payload.data() + cursor), 4);
      if (version_major == 4) {
        frame_size = (static_cast<uint32_t>(payload[cursor + 4] & 0x7F) << 21) |
                     (static_cast<uint32_t>(payload[cursor + 5] & 0x7F) << 14) |
                     (static_cast<uint32_t>(payload[cursor + 6] & 0x7F) << 7) |
                     static_cast<uint32_t>(payload[cursor + 7] & 0x7F);
        uint16_t frame_flags = load_u16_be(payload.data() + cursor + 8);
        frame_unsync = (frame_flags & 0x0002) != 0;
        data_length_indicator = (frame_flags & 0x0001) != 0;
      } else {
        frame_size = load_u32_be(payload.data() + cursor + 4);
      }
      header_len = 10;
    }

    cursor += header_len;
    if (cursor + frame_size > payload.size()) {
      frame_size = static_cast<uint32_t>(payload.size() - cursor);
    }

    std::span<const uint8_t> frame_data = payload.subspan(cursor, frame_size);
    cursor += frame_size;

    std::vector<uint8_t> deunsynced_frame;
    if (frame_unsync) {
      deunsynced_frame = deunsynchronize(frame_data);
      frame_data = deunsynced_frame;
    }

    if (data_length_indicator && frame_data.size() >= 4) {
      frame_data = frame_data.subspan(4);
    }

    if (frame_data.empty()) continue;

    uint8_t encoding = frame_data[0];
    std::span<const uint8_t> frame_body = frame_data.subspan(1);

    if (frame_id == "TIT2" || frame_id == "TT2") {
      metadata.setString(TITLE, decodeId3Text(encoding, frame_body));
    } else if (frame_id == "TPE1" || frame_id == "TP1") {
      metadata.setString(ARTIST, decodeId3Text(encoding, frame_body));
    } else if (frame_id == "TPE2" || frame_id == "TP2") {
      metadata.setString(ALBUM_ARTIST, decodeId3Text(encoding, frame_body));
    } else if (frame_id == "TALB" || frame_id == "TAL") {
      metadata.setString(ALBUM, decodeId3Text(encoding, frame_body));
    } else if (frame_id == "TYER" || frame_id == "TYE" || frame_id == "TDRC" || frame_id == "TDRL") {
      if (!metadata.contains(DATE)) {
        metadata.setString(DATE, decodeId3Text(encoding, frame_body));
      }
    } else if (frame_id == "TCON" || frame_id == "TCO") {
      metadata.setString(GENRE, resolveId3Genre(decodeId3Text(encoding, frame_body)));
    } else if (frame_id == "TCOM" || frame_id == "TCM") {
      metadata.setString(COMPOSER, decodeId3Text(encoding, frame_body));
    } else if (frame_id == "TSSE" || frame_id == "TSS" || frame_id == "TENC" || frame_id == "TEN") {
      metadata.setString(ENCODER, decodeId3Text(encoding, frame_body));
    } else if (frame_id == "TCOP" || frame_id == "TCR") {
      metadata.setString(COPYRIGHT, decodeId3Text(encoding, frame_body));
    } else if (frame_id == "TPUB" || frame_id == "TPB") {
      metadata.setString(PUBLISHER, decodeId3Text(encoding, frame_body));
    } else if (frame_id == "TLAN" || frame_id == "TLA") {
      metadata.setString(LANGUAGE, decodeId3Text(encoding, frame_body));
    } else if (frame_id == "TRCK" || frame_id == "TRK") {
      parseNumberPair(decodeId3Text(encoding, frame_body), TRACK_NUMBER, TRACK_TOTAL, metadata);
    } else if (frame_id == "TPOS" || frame_id == "TPA") {
      parseNumberPair(decodeId3Text(encoding, frame_body), DISC_NUMBER, DISC_TOTAL, metadata);
    } else if (frame_id == "TBPM" || frame_id == "TBP") {
      std::string s = decodeId3Text(encoding, frame_body);
      int bpm = 0;
      auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), bpm);
      if (ec == std::errc {} && bpm > 0) {
        metadata.setInt32(BPM, bpm);
      }
    } else if (frame_id == "TCMP" || frame_id == "TCP") {
      metadata.setBool(COMPILATION, decodeId3Text(encoding, frame_body) == "1");
    } else if (frame_id == "TSOT" || frame_id == "TST") {
      metadata.setString(SORT_TITLE, decodeId3Text(encoding, frame_body));
    } else if (frame_id == "TSOP" || frame_id == "TSP") {
      metadata.setString(SORT_ARTIST, decodeId3Text(encoding, frame_body));
    } else if (frame_id == "TSOA" || frame_id == "TSA") {
      metadata.setString(SORT_ALBUM, decodeId3Text(encoding, frame_body));
    } else if (frame_id == "TSO2") {
      metadata.setString(SORT_ALBUM_ARTIST, decodeId3Text(encoding, frame_body));
    } else if (frame_id == "COMM" || frame_id == "COM") {
      if (frame_body.size() >= 3) {
        size_t desc_start = 3;
        size_t desc_end = findNullTerminator(encoding, frame_body, desc_start);
        size_t term_len = nullTerminatorLength(encoding);
        size_t text_start = std::min(frame_body.size(), desc_end + term_len);
        metadata.setString(COMMENT, decodeId3Text(encoding, frame_body.subspan(text_start)));
      }
    } else if (frame_id == "USLT" || frame_id == "ULT") {
      if (frame_body.size() >= 3) {
        size_t desc_start = 3;
        size_t desc_end = findNullTerminator(encoding, frame_body, desc_start);
        size_t term_len = nullTerminatorLength(encoding);
        size_t text_start = std::min(frame_body.size(), desc_end + term_len);
        metadata.setString(LYRICS, decodeId3Text(encoding, frame_body.subspan(text_start)));
      }
    } else if (frame_id == "TXXX" || frame_id == "TXX") {
      size_t desc_end = findNullTerminator(encoding, frame_body, 0);
      std::string desc = decodeId3Text(encoding, frame_body.subspan(0, desc_end));
      size_t term_len = nullTerminatorLength(encoding);
      size_t val_start = std::min(frame_body.size(), desc_end + term_len);
      std::string val = decodeId3Text(encoding, frame_body.subspan(val_start));
      if (iequals(desc, "ALBUM ARTIST") || iequals(desc, "ALBUMARTIST")) {
        metadata.setString(ALBUM_ARTIST, val);
      } else if (iequals(desc, "COMPILATION")) {
        metadata.setBool(COMPILATION, val == "1");
      } else if (iequals(desc, "TOTALTRACKS") || iequals(desc, "TRACKTOTAL")) {
        int total = 0;
        auto [p, ec] = std::from_chars(val.data(), val.data() + val.size(), total);
        if (ec == std::errc {} && total > 0) metadata.setInt32(TRACK_TOTAL, total);
      } else if (iequals(desc, "TOTALDISCS") || iequals(desc, "DISCTOTAL")) {
        int total = 0;
        auto [p, ec] = std::from_chars(val.data(), val.data() + val.size(), total);
        if (ec == std::errc {} && total > 0) metadata.setInt32(DISC_TOTAL, total);
      } else if (iequals(desc, "BPM")) {
        int bpm = 0;
        auto [p, ec] = std::from_chars(val.data(), val.data() + val.size(), bpm);
        if (ec == std::errc {} && bpm > 0) metadata.setInt32(BPM, bpm);
      }
    } else if (frame_id == "APIC") {
      size_t mime_end = findNullTerminator(0, frame_body, 0);
      std::string mime = latin1ToUtf8(frame_body.subspan(0, mime_end));
      size_t pic_type_pos = mime_end + 1;
      if (pic_type_pos < frame_body.size()) {
        uint8_t pic_type = frame_body[pic_type_pos];
        size_t desc_start = pic_type_pos + 1;
        size_t desc_end = findNullTerminator(encoding, frame_body, desc_start);
        size_t term_len = nullTerminatorLength(encoding);
        size_t pic_data_pos = std::min(frame_body.size(), desc_end + term_len);
        if (pic_data_pos < frame_body.size()) {
          std::span<const uint8_t> pic_data = frame_body.subspan(pic_data_pos);
          if (pic_type == 3 || !metadata.contains(COVER_ART)) {
            metadata.setBinary(COVER_ART, pic_data);
            if (!mime.empty() && mime != "-->") {
              metadata.setString(COVER_ART_MIME, mime);
            }
          }
        }
      }
    } else if (frame_id == "PIC") {
      if (frame_body.size() >= 4) {
        std::string_view fmt(reinterpret_cast<const char*>(frame_body.data()), 3);
        uint8_t pic_type = frame_body[3];
        size_t desc_start = 4;
        size_t desc_end = findNullTerminator(encoding, frame_body, desc_start);
        size_t term_len = nullTerminatorLength(encoding);
        size_t pic_data_pos = std::min(frame_body.size(), desc_end + term_len);
        if (pic_data_pos < frame_body.size()) {
          std::span<const uint8_t> pic_data = frame_body.subspan(pic_data_pos);
          if (pic_type == 3 || !metadata.contains(COVER_ART)) {
            metadata.setBinary(COVER_ART, pic_data);
            if (iequals(fmt, "JPG")) {
              metadata.setString(COVER_ART_MIME, "image/jpeg");
            } else if (iequals(fmt, "PNG")) {
              metadata.setString(COVER_ART_MIME, "image/png");
            }
          }
        }
      }
    }
  }
}

inline auto cleanId3v1String(std::span<const uint8_t> bytes) -> std::string {
  size_t len = bytes.size();
  while (len > 0 && (bytes[len - 1] == 0 || std::isspace(bytes[len - 1]))) {
    --len;
  }
  return latin1ToUtf8(bytes.subspan(0, len));
}

inline void parseId3v1(std::span<const uint8_t> data, Dictionary& metadata) {
  if (data.size() < 128) return;
  const uint8_t* ptr = data.data() + data.size() - 128;
  if (std::memcmp(ptr, "TAG", 3) != 0) return;

  std::string title = cleanId3v1String(std::span<const uint8_t>(ptr + 3, 30));
  std::string artist = cleanId3v1String(std::span<const uint8_t>(ptr + 33, 30));
  std::string album = cleanId3v1String(std::span<const uint8_t>(ptr + 63, 30));
  std::string year = cleanId3v1String(std::span<const uint8_t>(ptr + 93, 4));

  std::string comment;
  int track = 0;
  if (ptr[125] == 0 && ptr[126] != 0) {
    comment = cleanId3v1String(std::span<const uint8_t>(ptr + 97, 28));
    track = ptr[126];
  } else {
    comment = cleanId3v1String(std::span<const uint8_t>(ptr + 97, 30));
  }
  uint8_t genre_idx = ptr[127];

  if (!metadata.contains(TITLE) && !title.empty()) metadata.setString(TITLE, title);
  if (!metadata.contains(ARTIST) && !artist.empty()) metadata.setString(ARTIST, artist);
  if (!metadata.contains(ALBUM) && !album.empty()) metadata.setString(ALBUM, album);
  if (!metadata.contains(DATE) && !year.empty()) metadata.setString(DATE, year);
  if (!metadata.contains(COMMENT) && !comment.empty()) metadata.setString(COMMENT, comment);
  if (!metadata.contains(TRACK_NUMBER) && track > 0) metadata.setInt32(TRACK_NUMBER, track);
  if (!metadata.contains(GENRE)) {
    auto genre_name = id3v1GenreName(genre_idx);
    if (!genre_name.empty()) {
      metadata.setString(GENRE, genre_name);
    }
  }
}

}
