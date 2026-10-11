#pragma once

#include <cassert>
#include <functional>
#include <type_traits>
#include <utility>
#include <variant>

namespace openmedia {

template<class T>
struct OkTag {
  T value;
};

template<class E>
struct ErrTag {
  E value;
};

template<class T>
constexpr auto Ok(T&& value) noexcept -> OkTag<T&&> {
  return {std::forward<T>(value)};
}

template<class E>
constexpr auto Err(E&& error) noexcept -> ErrTag<E&&> {
  return {std::forward<E>(error)};
}

template<class T, class E>
class Result;

template<class R>
struct is_result : std::false_type {};

template<class T, class E>
struct is_result<Result<T, E>> : std::true_type {};

template<class T, class E>
class Result {
  template<class K>
  using storage_t = std::conditional_t<std::is_reference_v<K>,
                                       std::reference_wrapper<std::remove_reference_t<K>>,
                                       K>;

  using ok_storage_t = storage_t<T>;
  using err_storage_t = storage_t<E>;

  std::variant<ok_storage_t, err_storage_t> storage_;

  template<class K, class U>
  static constexpr void checkNoDangling() {
    static_assert(!std::is_reference_v<K> || std::is_lvalue_reference_v<U>,
                  "Result<T&, ...> cannot bind to a temporary");
  }

  template<class K>
  static constexpr auto unref(K& v) noexcept -> K& { return v; }
  template<class K>
  static constexpr auto unref(std::reference_wrapper<K>& v) noexcept -> K& { return v.get(); }
  template<class K>
  static constexpr auto unref(const std::reference_wrapper<K>& v) noexcept -> K& { return v.get(); }

  constexpr auto& okSlot() noexcept { return *std::get_if<0>(&storage_); }
  constexpr auto& okSlot() const noexcept { return *std::get_if<0>(&storage_); }
  constexpr auto& errSlot() noexcept { return *std::get_if<1>(&storage_); }
  constexpr auto& errSlot() const noexcept { return *std::get_if<1>(&storage_); }

  template<std::size_t I, class U>
  constexpr Result(std::in_place_index_t<I> idx, U&& v) : storage_(idx, std::forward<U>(v)) {}

public:
  using ok_type = T;
  using err_type = E;

  Result() = delete;

  template<class U = T>
  static constexpr auto makeOk(U&& value) -> Result {
    checkNoDangling<T, U&&>();
    return Result(std::in_place_index<0>, std::forward<U>(value));
  }

  template<class U = E>
  static constexpr auto makeErr(U&& error) -> Result {
    checkNoDangling<E, U&&>();
    return Result(std::in_place_index<1>, std::forward<U>(error));
  }

  template<class U, std::enable_if_t<std::is_constructible_v<ok_storage_t, U&&>, int> = 0>
  constexpr Result(OkTag<U> tag) : storage_(std::in_place_index<0>, std::forward<U>(tag.value)) {
    checkNoDangling<T, U>();
  }

  template<class U, std::enable_if_t<std::is_constructible_v<err_storage_t, U&&>, int> = 0>
  constexpr Result(ErrTag<U> tag) : storage_(std::in_place_index<1>, std::forward<U>(tag.value)) {
    checkNoDangling<E, U>();
  }

  constexpr auto isOk() const noexcept -> bool { return storage_.index() == 0; }
  constexpr auto isErr() const noexcept -> bool { return storage_.index() == 1; }

  constexpr explicit operator bool() const noexcept { return isOk(); }

  constexpr auto unwrap() & noexcept -> T& {
    assert(isOk() && "Result::unwrap() called on Err");
    return unref(okSlot());
  }

  constexpr auto unwrap() const& noexcept -> const T& {
    assert(isOk() && "Result::unwrap() called on Err");
    return unref(okSlot());
  }

  constexpr auto unwrap() && -> T {
    assert(isOk() && "Result::unwrap() called on Err");
    if constexpr (std::is_reference_v<T>) {
      return unref(okSlot());
    } else {
      return std::move(okSlot());
    }
  }

  constexpr auto unwrapErr() & noexcept -> E& {
    assert(isErr() && "Result::unwrapErr() called on Ok");
    return unref(errSlot());
  }

  constexpr auto unwrapErr() const& noexcept -> const E& {
    assert(isErr() && "Result::unwrapErr() called on Ok");
    return unref(errSlot());
  }

  constexpr auto unwrapErr() && -> E {
    assert(isErr() && "Result::unwrapErr() called on Ok");
    if constexpr (std::is_reference_v<E>) {
      return unref(errSlot());
    } else {
      return std::move(errSlot());
    }
  }

  template<class U>
  constexpr auto unwrapOr(U&& fallback) const& -> T {
    if (isOk()) return unwrap();
    return static_cast<T>(std::forward<U>(fallback));
  }

  template<class U>
  constexpr auto unwrapOr(U&& fallback) && -> T {
    if (isOk()) return std::move(*this).unwrap();
    return static_cast<T>(std::forward<U>(fallback));
  }

  template<class F>
  constexpr auto unwrapOrElse(F&& fn) const& -> T {
    if (isOk()) return unwrap();
    return std::invoke(std::forward<F>(fn), unwrapErr());
  }

  template<class F>
  constexpr auto unwrapOrElse(F&& fn) && -> T {
    if (isOk()) return std::move(*this).unwrap();
    return std::invoke(std::forward<F>(fn), std::move(*this).unwrapErr());
  }

private:
  template<class Self, class F>
  static constexpr auto mapImpl(Self&& self, F&& fn) {
    using U = std::invoke_result_t<F, decltype(std::forward<Self>(self).unwrap())>;
    using R = Result<U, E>;
    if (self.isOk()) return R::makeOk(std::invoke(std::forward<F>(fn), std::forward<Self>(self).unwrap()));
    return R::makeErr(std::forward<Self>(self).unwrapErr());
  }

  template<class Self, class F>
  static constexpr auto mapErrImpl(Self&& self, F&& fn) {
    using G = std::invoke_result_t<F, decltype(std::forward<Self>(self).unwrapErr())>;
    using R = Result<T, G>;
    if (self.isErr()) return R::makeErr(std::invoke(std::forward<F>(fn), std::forward<Self>(self).unwrapErr()));
    return R::makeOk(std::forward<Self>(self).unwrap());
  }

  template<class Self, class F>
  static constexpr auto andThenImpl(Self&& self, F&& fn) {
    using R = std::invoke_result_t<F, decltype(std::forward<Self>(self).unwrap())>;
    static_assert(is_result<std::remove_cv_t<std::remove_reference_t<R>>>::value,
                  "andThen: callback must return a Result");
    if (self.isOk()) return std::invoke(std::forward<F>(fn), std::forward<Self>(self).unwrap());
    return R::makeErr(std::forward<Self>(self).unwrapErr());
  }

public:
  template<class F> constexpr auto map(F&& fn) & { return mapImpl(*this, std::forward<F>(fn)); }
  template<class F> constexpr auto map(F&& fn) const& { return mapImpl(*this, std::forward<F>(fn)); }
  template<class F> constexpr auto map(F&& fn) && { return mapImpl(std::move(*this), std::forward<F>(fn)); }

  template<class F> constexpr auto mapErr(F&& fn) & { return mapErrImpl(*this, std::forward<F>(fn)); }
  template<class F> constexpr auto mapErr(F&& fn) const& { return mapErrImpl(*this, std::forward<F>(fn)); }
  template<class F> constexpr auto mapErr(F&& fn) && { return mapErrImpl(std::move(*this), std::forward<F>(fn)); }

  template<class F> constexpr auto andThen(F&& fn) & { return andThenImpl(*this, std::forward<F>(fn)); }
  template<class F> constexpr auto andThen(F&& fn) const& { return andThenImpl(*this, std::forward<F>(fn)); }
  template<class F> constexpr auto andThen(F&& fn) && { return andThenImpl(std::move(*this), std::forward<F>(fn)); }
};

template<class T, class E>
Result(OkTag<T>, ErrTag<E>) -> Result<T, E>;

} // namespace openmedia
