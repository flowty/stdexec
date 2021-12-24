/*
 * Copyright (c) NVIDIA
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

// Pull in the reference implementation of P2300:
#include <execution.hpp>

template <class From, class To>
using _copy_cvref_t = std::__member_t<From, To>;

template <class From, class To>
concept _decays_to = std::same_as<std::decay_t<From>, To>;

namespace stdex = std::execution;

///////////////////////////////////////////////////////////////////////////////
// retry algorithm:

// _conv needed so we can emplace construct non-movable types into
// a std::optional.
template<std::invocable F>
  requires std::is_nothrow_move_constructible_v<F>
struct _conv {
  F f_;
  explicit _conv(F f) noexcept : f_((F&&) f) {}
  operator std::invoke_result_t<F>() && {
    return ((F&&) f_)();
  }
};

template<class S, class R, class C>
struct _op;

// pass through all customizations except set_error, which retries the operation.
template<class S, class R, class C>
struct _retry_receiver
  : stdex::receiver_adaptor<_retry_receiver<S, R, C>> {
  _op<S, R, C>* o_;

  R&& base() && noexcept { return (R&&) o_->r_; }
  const R& base() const & noexcept { return o_->r_; }

  explicit _retry_receiver(_op<S, R, C>* o) : o_(o) {}

  void set_error(auto&&) && noexcept {
    o_->_retry(); // This causes the op to be retried
  }
};

// Hold the nested operation state in an optional so we can
// re-construct and re-start it if the operation fails.
template<class S, class R, class C>
struct _op {
  S s_;
  R r_;
  C c_;
  std::optional<
      stdex::connect_result_t<S&, _retry_receiver<S, R, C>, C&>> o_;

  _op(S s, R r, C c): s_((S&&)s), r_((R&&)r), c_((C&&)c), o_{_connect()} {}
  _op(_op&&) = delete;

  auto _connect() noexcept {
    return _conv{[this] {
      return stdex::connect(s_, _retry_receiver<S, R, C>{this}, c_);
    }};
  }
  void _retry() noexcept try {
    o_.emplace(_connect()); // potentially throwing
    stdex::start(*o_);
  } catch(...) {
    stdex::set_error((R&&) r_, std::current_exception());
  }
  friend void tag_invoke(stdex::start_t, _op& o) noexcept {
    stdex::start(*o.o_);
  }
};

template<class S>
struct _retry_sender {
  S s_;
  explicit _retry_sender(S s) : s_((S&&) s) {}

  template<stdex::receiver R, class C>
    requires stdex::sender_to<S&, R, C&>
  friend _op<S, R, C> tag_invoke(stdex::connect_t, _retry_sender&& self, R r, C c) {
    return {(S&&) self.s_, (R&&) r, (C &&) c};
  }

  template <class Context>
  friend auto tag_invoke(stdex::get_sender_traits_t, const _retry_sender&, Context)
    -> stdex::sender_traits_t<const S&, Context>;
};

template<stdex::sender S>
stdex::sender auto retry(S s) {
  return _retry_sender{(S&&) s};
}
