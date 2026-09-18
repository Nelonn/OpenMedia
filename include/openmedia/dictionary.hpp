#pragma once

#include <charconv>
#include <cstdint>
#include <cstring>
#include <memory>
#include <openmedia/macro.h>
#include <openmedia/media.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace openmedia {

constexpr auto fnv1a_hash(const char* str, size_t n, uint64_t hash = 0xcbf29ce484222325ULL) -> uint64_t {
  return n == 0 ? hash : fnv1a_hash(str + 1, n - 1, (hash ^ static_cast<uint64_t>(str[0])) * 0x100000001b3ULL);
}

constexpr auto hash_ct(const char* str) -> uint64_t {
  size_t len = 0;
  while (str[len]) {
    ++len;
  }
  return fnv1a_hash(str, len);
}

class Dictionary;

/**
 * @brief Value type enumeration for dictionary values
 */
enum class ValueType : uint8_t {
  NONE = 0,
  INT32,
  INT64,
  FLOAT,
  DOUBLE,
  BOOL,
  STRING,
  RATIONAL,
  BINARY,
  ARRAY,
  DICT,
};

/**
 * @brief Key class with compile-time hash computation
 *
 * Stores a string_view and a precomputed hash for fast lookups.
 * The hash is computed at compile time when using the constexpr constructor.
 */
class OPENMEDIA_ABI Key {
  std::string_view view_;
  uint64_t hash_;

public:
  constexpr Key() noexcept
      : view_(), hash_(0) {}

  constexpr Key(std::string_view view) noexcept
      : view_(view), hash_(computeHash(view)) {}

  constexpr Key(const char* str) noexcept
      : view_(str), hash_(hash_ct(str)) {}

  constexpr Key(const char* str, size_t len) noexcept
      : view_(str, len), hash_(fnv1a_hash(str, len)) {}

  constexpr Key(const std::string& str) noexcept
      : view_(str), hash_(computeHash(view_)) {}

  constexpr Key(const Key&) noexcept = default;
  constexpr auto operator=(const Key&) noexcept -> Key& = default;
  constexpr Key(Key&&) noexcept = default;
  constexpr auto operator=(Key&&) noexcept -> Key& = default;

  constexpr auto view() const noexcept -> std::string_view { return view_; }
  constexpr auto data() const noexcept -> const char* { return view_.data(); }
  constexpr auto size() const noexcept -> size_t { return view_.size(); }
  constexpr auto hash() const noexcept -> uint64_t { return hash_; }
  constexpr auto empty() const noexcept -> bool { return view_.empty(); }

  constexpr auto operator==(const Key& other) const noexcept -> bool {
    return hash_ == other.hash_ && view_ == other.view_;
  }
  constexpr auto operator!=(const Key& other) const noexcept -> bool { return !(*this == other); }
  constexpr auto operator<(const Key& other) const noexcept -> bool { return view_ < other.view_; }
  constexpr auto operator<=(const Key& other) const noexcept -> bool { return view_ <= other.view_; }
  constexpr auto operator>(const Key& other) const noexcept -> bool { return view_ > other.view_; }
  constexpr auto operator>=(const Key& other) const noexcept -> bool { return view_ >= other.view_; }

  constexpr explicit operator std::string_view() const noexcept { return view_; }
  constexpr explicit operator bool() const noexcept { return !view_.empty(); }

  auto toString() const -> std::string { return std::string(view_); }

private:
  static constexpr auto computeHash(std::string_view view) noexcept -> uint64_t {
    return fnv1a_hash(view.data(), view.size());
  }
};

/**
 * @brief Owning binary blob.
 *
 * A dictionary outlives whatever produced the value put into it — a demuxer
 * hands over bytes it parsed out of a buffer it is about to drop — so this
 * copies on construction rather than referring to the caller's storage.
 */
struct OPENMEDIA_ABI BinaryData {
  std::vector<uint8_t> bytes;

  BinaryData() = default;
  BinaryData(std::span<const uint8_t> d)
      : bytes(d.begin(), d.end()) {}

  template<size_t N>
  BinaryData(const uint8_t (&arr)[N])
      : bytes(arr, arr + N) {}

  auto span() const noexcept -> std::span<const uint8_t> { return bytes; }
  auto data() const noexcept -> const uint8_t* { return bytes.data(); }
  auto size() const noexcept -> size_t { return bytes.size(); }
  auto empty() const noexcept -> bool { return bytes.empty(); }

  auto operator==(const BinaryData& other) const noexcept -> bool {
    return bytes == other.bytes;
  }

  auto operator!=(const BinaryData& other) const noexcept -> bool {
    return !(*this == other);
  }
};

/**
 * @brief Value class that can hold any supported type
 *
 * Uses std::variant for type-safe storage of different value types.
 */
class OPENMEDIA_ABI Value {
public:
  using ArrayType = std::vector<Value>;

private:
  using VariantType = std::variant<
      std::monostate,              // NONE
      int32_t,                     // INT32
      int64_t,                     // INT64
      float,                       // FLOAT
      double,                      // DOUBLE
      std::string,                 // STRING
      Rational,                  // RATIONAL
      std::shared_ptr<BinaryData>, // BINARY
      bool,                        // BOOL
      std::shared_ptr<ArrayType>,  // ARRAY
      std::shared_ptr<Dictionary>  // DICT
      >;

  VariantType data_;

  static constexpr auto variantIndexToType(size_t index) -> ValueType {
    return static_cast<ValueType>(index);
  }

public:
  constexpr Value() noexcept = default;

  constexpr Value(int32_t v) noexcept
      : data_(v) {}
  constexpr Value(int64_t v) noexcept
      : data_(v) {}
  constexpr Value(float v) noexcept
      : data_(v) {}
  constexpr Value(double v) noexcept
      : data_(v) {}
  Value(const char* v)
      : data_(std::string(v ? v : "")) {}
  Value(const std::string& v)
      : data_(v) {}
  Value(std::string&& v)
      : data_(std::move(v)) {}
  constexpr Value(Rational v) noexcept
      : data_(v) {}
  Value(std::span<const uint8_t> d)
      : data_(std::make_shared<BinaryData>(BinaryData {d})) {}
  Value(BinaryData bd)
      : data_(std::make_shared<BinaryData>(std::move(bd))) {}
  constexpr Value(bool v) noexcept
      : data_(v) {}

  template<typename T>
  Value(std::initializer_list<T> list)
      : data_(std::make_shared<ArrayType>(list.begin(), list.end())) {}

  Value(const ArrayType& arr)
      : data_(std::make_shared<ArrayType>(arr)) {}
  Value(ArrayType&& arr)
      : data_(std::make_shared<ArrayType>(std::move(arr))) {}

  Value(const Dictionary& dict)
      : data_(std::make_shared<Dictionary>(dict)) {}
  Value(std::shared_ptr<Dictionary> dict)
      : data_(std::move(dict)) {}

  Value(const Value&) = default;
  auto operator=(const Value&) -> Value& = default;
  Value(Value&&) noexcept = default;
  auto operator=(Value&&) noexcept -> Value& = default;

  auto type() const noexcept -> ValueType { return variantIndexToType(data_.index()); }

  auto isNone() const noexcept -> bool { return data_.index() == static_cast<size_t>(ValueType::NONE); }
  auto isInt32() const noexcept -> bool { return data_.index() == static_cast<size_t>(ValueType::INT32); }
  auto isInt64() const noexcept -> bool { return data_.index() == static_cast<size_t>(ValueType::INT64); }
  auto isFloat() const noexcept -> bool { return data_.index() == static_cast<size_t>(ValueType::FLOAT); }
  auto isDouble() const noexcept -> bool { return data_.index() == static_cast<size_t>(ValueType::DOUBLE); }
  auto isString() const noexcept -> bool { return data_.index() == static_cast<size_t>(ValueType::STRING); }
  auto isRational() const noexcept -> bool { return data_.index() == static_cast<size_t>(ValueType::RATIONAL); }
  auto isBinary() const noexcept -> bool { return data_.index() == static_cast<size_t>(ValueType::BINARY); }
  auto isBool() const noexcept -> bool { return data_.index() == static_cast<size_t>(ValueType::BOOL); }
  auto isArray() const noexcept -> bool { return data_.index() == static_cast<size_t>(ValueType::ARRAY); }
  auto isDict() const noexcept -> bool { return data_.index() == static_cast<size_t>(ValueType::DICT); }

  auto getInt32() const noexcept -> std::optional<int32_t> {
    if (auto* v = std::get_if<int32_t>(&data_)) return *v;
    return std::nullopt;
  }
  auto getInt64() const noexcept -> std::optional<int64_t> {
    if (auto* v = std::get_if<int64_t>(&data_)) return *v;
    return std::nullopt;
  }
  auto getFloat() const noexcept -> std::optional<float> {
    if (auto* v = std::get_if<float>(&data_)) return *v;
    return std::nullopt;
  }
  auto getDouble() const noexcept -> std::optional<double> {
    if (auto* v = std::get_if<double>(&data_)) return *v;
    return std::nullopt;
  }
  auto getString() const noexcept -> std::optional<std::string_view> {
    if (auto* v = std::get_if<std::string>(&data_)) return std::string_view(*v);
    return std::nullopt;
  }
  auto getStringRef() const -> const std::string& { return std::get<std::string>(data_); }
  auto getRational() const noexcept -> std::optional<Rational> {
    if (auto* v = std::get_if<Rational>(&data_)) return *v;
    return std::nullopt;
  }
  // A view into the stored blob, valid for as long as this Value is. Returning
  // the BinaryData itself would copy every byte on each read, which for cover
  // art is megabytes.
  auto getBinary() const noexcept -> std::optional<std::span<const uint8_t>> {
    if (auto* v = std::get_if<std::shared_ptr<BinaryData>>(&data_)) {
      if (*v) return (*v)->span();
    }
    return std::nullopt;
  }
  auto getBool() const noexcept -> std::optional<bool> {
    if (auto* v = std::get_if<bool>(&data_)) return *v;
    return std::nullopt;
  }
  auto getArray() const noexcept -> std::optional<std::reference_wrapper<const ArrayType>> {
    if (auto* v = std::get_if<std::shared_ptr<ArrayType>>(&data_)) {
      if (*v) return std::cref(**v);
    }
    return std::nullopt;
  }
  auto getArrayRef() const -> const ArrayType& {
    return *std::get<std::shared_ptr<ArrayType>>(data_);
  }
  auto getDict() const noexcept -> std::optional<std::reference_wrapper<const Dictionary>> {
    if (auto* v = std::get_if<std::shared_ptr<Dictionary>>(&data_)) {
      if (*v) return std::cref(**v);
    }
    return std::nullopt;
  }
  auto getDictRef() const -> const Dictionary& {
    return *std::get<std::shared_ptr<Dictionary>>(data_);
  }

  template<typename T>
  auto get() const -> T { return std::get<T>(data_); }

  template<typename T>
  auto getPtr() const noexcept -> const T* { return std::get_if<T>(&data_); }

  auto toInt64() const noexcept -> std::optional<int64_t> {
    switch (data_.index()) {
      case 1: return std::get<int32_t>(data_);
      case 2: return std::get<int64_t>(data_);
      case 3: return static_cast<int64_t>(std::get<float>(data_));
      case 4: return static_cast<int64_t>(std::get<double>(data_));
      case 5: {
        const auto& s = std::get<std::string>(data_);
        int64_t result {};
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
        if (ec == std::errc {}) return result;
        return std::nullopt;
      }
      case 8: return std::get<bool>(data_) ? 1 : 0;
      default: return std::nullopt;
    }
  }

  auto toDouble() const noexcept -> std::optional<double> {
    switch (data_.index()) {
      case 1: return static_cast<double>(std::get<int32_t>(data_));
      case 2: return static_cast<double>(std::get<int64_t>(data_));
      case 3: return static_cast<double>(std::get<float>(data_));
      case 4: return std::get<double>(data_);
      case 5: {
        const auto& s = std::get<std::string>(data_);
        double result {};
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
        if (ec == std::errc {}) return result;
        return std::nullopt;
      }
      case 6: return std::get<Rational>(data_).toDouble();
      case 8: return std::get<bool>(data_) ? 1.0 : 0.0;
      default: return std::nullopt;
    }
  }

  auto toString() const -> std::string {
    switch (data_.index()) {
      case 0: return "";
      case 1: return std::to_string(std::get<int32_t>(data_));
      case 2: return std::to_string(std::get<int64_t>(data_));
      case 3: return std::to_string(std::get<float>(data_));
      case 4: return std::to_string(std::get<double>(data_));
      case 5: return std::get<std::string>(data_);
      case 6: {
        const auto& r = std::get<Rational>(data_);
        return std::to_string(r.num) + "/" + std::to_string(r.den);
      }
      case 8: return std::get<bool>(data_) ? "true" : "false";
      default: return "<unsupported type>";
    }
  }

  auto operator==(const Value& other) const noexcept -> bool { return data_ == other.data_; }
  auto operator!=(const Value& other) const noexcept -> bool { return !(*this == other); }
};

/**
 * @brief Dictionary class for storing key-value pairs
 *
 * Open-addressing hash map with linear probing and tombstone deletion.
 */
class OPENMEDIA_ABI Dictionary {
public:
  using value_type = std::pair<const Key&, const Value&>;
  using size_type = size_t;

private:
  enum class SlotState : uint8_t {
    Empty,
    Occupied,
    Tombstone,
  };

  struct Entry {
    Key key;
    Value value;
    uint64_t hash = 0;
    SlotState state = SlotState::Empty;

    Entry() = default;
    Entry(const Key& k, const Value& v)
        : key(k), value(v), hash(k.hash()), state(SlotState::Occupied) {}
  };

  std::vector<Entry> entries_;
  size_t size_ = 0;       // occupied (non-tombstone) entries
  size_t tombstones_ = 0; // tombstone count

  // Round up to next power of two (minimum 8).
  static constexpr auto nextPow2(size_t n) noexcept -> size_t {
    if (n <= 8) return 8;
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
  }

  auto mask() const noexcept -> size_t { return entries_.size() - 1; }

  // Returns slot index for insertion (stops at first empty or tombstone, or
  // the existing matching key). Bounded by table size — cannot loop forever
  // because growIfNeeded guarantees load factor < 0.75.
  auto findSlotForInsert(const Key& key) const noexcept -> size_t {
    const size_t m = mask();
    size_t slot = key.hash() & m;
    size_t first_tombstone = entries_.size(); // sentinel

    for (size_t i = 0; i <= entries_.size(); ++i) {
      const auto& e = entries_[slot];
      if (e.state == SlotState::Empty) {
        // Prefer reusing a tombstone we passed earlier
        return first_tombstone < entries_.size() ? first_tombstone : slot;
      }
      if (e.state == SlotState::Tombstone) {
        if (first_tombstone >= entries_.size()) first_tombstone = slot;
      } else if (e.hash == key.hash() && e.key == key) {
        return slot; // update existing
      }
      slot = (slot + 1) & m;
    }
    // Should never reach here if load factor is maintained
    return entries_.size();
  }

  // Returns slot index of an existing key, or entries_.size() if not found.
  auto findSlotExisting(const Key& key) const noexcept -> size_t {
    if (entries_.empty()) return 0; // size() == 0, caller must handle
    const size_t m = mask();
    size_t slot = key.hash() & m;

    for (size_t i = 0; i < entries_.size(); ++i) {
      const auto& e = entries_[slot];
      if (e.state == SlotState::Empty) break; // key definitely not present
      if (e.state == SlotState::Occupied &&
          e.hash == key.hash() && e.key == key) {
        return slot;
      }
      slot = (slot + 1) & m;
    }
    return entries_.size(); // not found
  }

  // Rebuild the table at new_capacity (must be power of two).
  // Uses a separate local table to avoid re-entrant calls to set().
  void rehash(size_t new_capacity) {
    std::vector<Entry> old_entries(std::move(entries_));
    entries_.assign(new_capacity, Entry {});
    size_ = 0;
    tombstones_ = 0;

    for (auto& e : old_entries) {
      if (e.state == SlotState::Occupied) {
        auto slot = findSlotForInsert(e.key);
        entries_[slot] = std::move(e);
        entries_[slot].state = SlotState::Occupied;
        ++size_;
      }
    }
  }

  // Grow when (size + tombstones) / capacity >= 0.75.
  void growIfNeeded() {
    if (entries_.empty()) {
      entries_.assign(8, Entry {});
      return;
    }
    if ((size_ + tombstones_) * 4 >= entries_.size() * 3) {
      rehash(entries_.size() * 2);
    }
  }

public:
  Dictionary() = default;

  explicit Dictionary(size_t reserved) {
    if (reserved > 0) {
      entries_.assign(nextPow2(reserved), Entry {});
    }
  }

  Dictionary(const Dictionary&) = default;
  auto operator=(const Dictionary&) -> Dictionary& = default;
  Dictionary(Dictionary&&) noexcept = default;
  auto operator=(Dictionary&&) noexcept -> Dictionary& = default;

  auto size() const noexcept -> size_t { return size_; }
  auto empty() const noexcept -> bool { return size_ == 0; }
  auto capacity() const noexcept -> size_t { return entries_.size(); }

  void clear() {
    entries_.clear();
    size_ = 0;
    tombstones_ = 0;
  }

  void reserve(size_t n) {
    const size_t needed = nextPow2(n);
    if (needed > entries_.size()) rehash(needed);
  }

  auto set(const Key& key, const Value& value) -> Value& {
    growIfNeeded();
    auto slot = findSlotForInsert(key);
    if (entries_[slot].state == SlotState::Occupied) {
      entries_[slot].value = value;
    } else {
      if (entries_[slot].state == SlotState::Tombstone) --tombstones_;
      entries_[slot] = Entry(key, value);
      ++size_;
    }
    return entries_[slot].value;
  }

  auto set(std::string_view key, const Value& value) -> Value& { return set(Key(key), value); }
  auto set(const char* key, const Value& value) -> Value& { return set(Key(key), value); }

  auto setInt32(const Key& key, int32_t v) -> Value& { return set(key, Value(v)); }
  auto setInt64(const Key& key, int64_t v) -> Value& { return set(key, Value(v)); }
  auto setFloat(const Key& key, float v) -> Value& { return set(key, Value(v)); }
  auto setDouble(const Key& key, double v) -> Value& { return set(key, Value(v)); }
  auto setString(const Key& key, const std::string& v) -> Value& { return set(key, Value(v)); }
  auto setString(const Key& key, std::string_view v) -> Value& { return set(key, Value(std::string(v))); }
  auto setRational(const Key& key, Rational v) -> Value& { return set(key, Value(v)); }
  auto setBinary(const Key& key, std::span<const uint8_t> d) -> Value& { return set(key, Value(d)); }
  auto setBool(const Key& key, bool v) -> Value& { return set(key, Value(v)); }

  auto get(const Key& key) const noexcept -> const Value* {
    if (entries_.empty()) return nullptr;
    auto slot = findSlotExisting(key);
    if (slot < entries_.size()) return &entries_[slot].value;
    return nullptr;
  }

  auto get(const Key& key) noexcept -> Value* {
    if (entries_.empty()) return nullptr;
    auto slot = findSlotExisting(key);
    if (slot < entries_.size()) return &entries_[slot].value;
    return nullptr;
  }

  auto get(std::string_view key) const noexcept -> const Value* { return get(Key(key)); }
  auto get(const char* key) const noexcept -> const Value* { return get(Key(key)); }

  auto getInt32(const Key& key, int32_t dv = 0) const noexcept -> int32_t {
    if (auto* v = get(key)) {
      if (auto r = v->getInt32()) return *r;
      if (auto r = v->toInt64()) return static_cast<int32_t>(*r);
    }
    return dv;
  }
  auto getInt64(const Key& key, int64_t dv = 0) const noexcept -> int64_t {
    if (auto* v = get(key)) {
      if (auto r = v->getInt64()) return *r;
      if (auto r = v->toInt64()) return *r;
    }
    return dv;
  }
  auto getFloat(const Key& key, float dv = 0.f) const noexcept -> float {
    if (auto* v = get(key)) {
      if (auto r = v->getFloat()) return *r;
      if (auto r = v->toDouble()) return static_cast<float>(*r);
    }
    return dv;
  }
  auto getDouble(const Key& key, double dv = 0.0) const noexcept -> double {
    if (auto* v = get(key)) {
      if (auto r = v->getDouble()) return *r;
      if (auto r = v->toDouble()) return *r;
    }
    return dv;
  }
  auto getString(const Key& key, std::string_view dv = "") const noexcept -> std::string {
    if (auto* v = get(key)) {
      if (auto r = v->getString()) return std::string(*r);
    }
    return std::string(dv);
  }
  auto getRational(const Key& key, Rational dv = {}) const noexcept -> Rational {
    if (auto* v = get(key)) {
      if (auto r = v->getRational()) return *r;
    }
    return dv;
  }
  auto getBool(const Key& key, bool dv = false) const noexcept -> bool {
    if (auto* v = get(key)) {
      if (auto r = v->getBool()) return *r;
      if (auto r = v->toInt64()) return *r != 0;
    }
    return dv;
  }
  auto getBinary(const Key& key) const noexcept -> std::optional<std::span<const uint8_t>> {
    if (auto* v = get(key)) return v->getBinary();
    return std::nullopt;
  }

  auto contains(const Key& key) const noexcept -> bool { return get(key) != nullptr; }
  auto contains(std::string_view key) const noexcept -> bool { return contains(Key(key)); }
  auto contains(const char* key) const noexcept -> bool { return contains(Key(key)); }

  auto remove(const Key& key) -> bool {
    if (entries_.empty()) return false;
    auto slot = findSlotExisting(key);
    if (slot >= entries_.size()) return false;

    entries_[slot].key = Key {};
    entries_[slot].value = Value {};
    entries_[slot].state = SlotState::Tombstone;
    --size_;
    ++tombstones_;
    return true;
  }

  auto remove(std::string_view key) -> bool { return remove(Key(key)); }

  auto operator[](const Key& key) -> Value& {
    growIfNeeded();
    auto slot = findSlotForInsert(key);
    if (entries_[slot].state != SlotState::Occupied) {
      if (entries_[slot].state == SlotState::Tombstone) --tombstones_;
      entries_[slot] = Entry(key, Value {});
      ++size_;
    }
    return entries_[slot].value;
  }

  auto operator[](std::string_view key) -> Value& { return (*this)[Key(key)]; }
  auto operator[](const char* key) -> Value& { return (*this)[Key(key)]; }

  struct Iterator {
    using iterator_category = std::forward_iterator_tag;
    using difference_type = std::ptrdiff_t;

    const Entry* entries_ = nullptr;
    size_t index_ = 0;
    size_t end_ = 0;

    Iterator() = default;
    Iterator(const Entry* entries, size_t index, size_t end)
        : entries_(entries), index_(index), end_(end) {
      advanceToOccupied();
    }

    auto operator*() const noexcept -> value_type {
      return {entries_[index_].key, entries_[index_].value};
    }

    auto operator++() -> Iterator& {
      ++index_;
      advanceToOccupied();
      return *this;
    }

    auto operator++(int) -> Iterator {
      Iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    auto operator==(const Iterator& other) const noexcept -> bool {
      return index_ == other.index_ && end_ == other.end_;
    }

    auto operator!=(const Iterator& other) const noexcept -> bool {
      return !(*this == other);
    }

    // Expose key/value directly so range-for structured bindings work:
    // for (auto [k, v] : dict) { ... }
    auto key() const noexcept -> const Key& { return entries_[index_].key; }
    auto value() const noexcept -> const Value& { return entries_[index_].value; }

  private:
    void advanceToOccupied() noexcept {
      while (index_ < end_ && entries_[index_].state != SlotState::Occupied) {
        ++index_;
      }
    }
  };

  auto begin() const noexcept -> Iterator {
    return Iterator(entries_.data(), 0, entries_.size());
  }

  auto end() const noexcept -> Iterator {
    return Iterator(entries_.data(), entries_.size(), entries_.size());
  }
};

} // namespace openmedia
