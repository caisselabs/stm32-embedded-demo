// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// A `region` is a named exclusive region for senders: only one sender
// may execute inside the region at a time. Use `within<Name>(s)`:
//
//     a | region::within<Name>(s) | b
//
// `s` may be a sender or a composed adaptor:
//
//   - if `s` is a sender it runs only when the upstream completes with a
//     value (as if wrapped in `async::seq(s)`); an upstream error or stop
//     skips `s` and propagates unchanged.
//   - if `s` is a composed adaptor the upstream completion (value, error,
//     or stop) is passed into it.
//
// Either way the region is acquired before `s` and released after,
// regardless of the upstream completion channel or the type of `s`:
//
//     a | within<Name>(s) | b   is as-if
//       a | acquire<Name> | async::seq(s) | release<Name> | b   (s a sender)
//       a | acquire<Name> |            s  | release<Name> | b   (s an adaptor)
//
// While a `within` is queued waiting for the region, a stop request on the
// receiver cancels it: it leaves the FIFO and completes stopped without ever
// acquiring — and so, having never held the region, without releasing.
//
// The release hands the region to the next waiter *before* forwarding the
// completion downstream, so anything piped after `within` runs once that
// waiter is already going. Any cleanup the next user depends on — resetting
// a peripheral, clearing error flags, returning a bus to idle — belongs
// inside `s`, on every channel `s` can complete on:
//
//     a | within<Name>(s) | recover | b   // too late
//     a | within<Name>(s | recover) | b   // correct
//
// A region serializes access; it does not restore state. See
// docs/region.org, "Leave the resource clean before you leave the region".
//
// The `acquire`/`release` primitives are implementation details living in
// `region::detail` and not part of the public interface.
//
// Waiters queue on a zero-allocation intrusive FIFO. Concurrency discipline:
// ALL region state — the waiter count, the queue, each node's `pending` bit,
// and each op's cancellation state — is guarded by the single per-name
// mutex, and every transition (acquire, enqueue, dequeue-on-cancel,
// release-and-choose-successor) updates everything it touches in ONE
// critical section. Splitting any of these across two sections lets another
// context observe a half-updated region (e.g. a count that includes a waiter
// the queue no longer holds) and strand a waiter or double-grant the region.
// Receiver completions always happen *outside* the critical section: they
// run arbitrary user code.
//
#pragma once

#include <async/completion_tags.hpp>
#include <async/compose.hpp>
#include <async/concepts.hpp>
#include <async/connect.hpp>
#include <async/debug.hpp>
#include <async/env.hpp>
#include <async/just.hpp>
#include <async/just_result_of.hpp>
#include <async/let.hpp>
#include <async/sequence.hpp>
#include <async/stop_token.hpp>
#include <async/type_traits.hpp>

#include <stdx/concepts.hpp>
#include <stdx/ct_string.hpp>
#include <stdx/intrusive_list.hpp>

#include <conc/concurrency.hpp>

#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace region {

struct sender_t;

namespace detail {

// FIFO node, one per queued acquire. `pending` is true exactly while the
// node is linked in the queue; like the queue itself it is guarded by
// mutex<Name>. It is the arbitration bit between cancellation and hand-off:
// whichever side clears it (in a critical section) owns the completion.
// NOLINTNEXTLINE(*-special-member-functions)
struct task_base {
   virtual auto run() -> void = 0;

   constexpr task_base() = default;
   constexpr task_base(task_base &&) = delete;

   task_base *prev{};
   task_base *next{};
   bool pending{};
};

template <stdx::ct_string Name>
struct mutex;
// waiters_ counts the holder plus all queued waiters; the region is free
// iff it is zero. Invariant at every critical-section boundary:
// waiters_ == (1 if held else 0) + queue_ length.
template <stdx::ct_string Name>
inline int waiters_{};
template <stdx::ct_string Name>
inline stdx::intrusive_list<task_base> queue_{};

// Stop-token plumbing and cancellation state for an acquire op.
//
// The stop callback is armed *before* the op is enqueued (see
// op_state::start), so a stop request may fire while the op is not yet
// queued. The tri-state below — guarded, like all region state, by
// mutex<Name> — reconciles that: a callback that finds the op not yet
// `queued` marks it `cancelled` and completes nothing; start() then observes
// the mark in its critical section and completes stopped without ever
// enqueueing.
//
// Specialized away to nothing when the receiver's token can never stop, so
// unstoppable receivers pay nothing.
template <typename Rcvr, typename Ops>
struct stop_base {
   auto check_stopped() -> bool { //
      return async::get_stop_token(async::get_env(self().rcvr)).stop_requested();
   }
   auto emplace_stop_cb() -> void {
      stop_cb.emplace(async::get_stop_token(async::get_env(self().rcvr)),
                      stop_cb_fn{std::addressof(self())});
   }
   auto clear_stop_cb() -> void { stop_cb.reset(); }

   // Cancellation state: all access under mutex<Name>.
   auto reset_cancel_state() -> void { state = status::init; }
   auto mark_queued() -> void { state = status::queued; }
   auto mark_cancelled() -> void { state = status::cancelled; }
   [[nodiscard]] auto queued() const -> bool { return state == status::queued; }
   [[nodiscard]] auto cancelled() const -> bool { return state == status::cancelled; }

 private:
   auto self() -> Ops & { return static_cast<Ops &>(*this); }

   struct stop_cb_fn {
      Ops *ops;
      auto operator()() -> void {
         if (ops->stop()) {
            ops->complete_stopped();
         }
      }
   };

   enum struct status : std::uint8_t { init, queued, cancelled };
   status state{status::init};

   using stop_token_t = async::stop_token_of_t<async::env_of_t<Rcvr>>;
   using stop_cb_t = async::stop_callback_for_t<stop_token_t, stop_cb_fn>;
   std::optional<stop_cb_t> stop_cb{};
};

template <typename Rcvr, typename Ops>
   requires async::unstoppable_token<async::stop_token_of_t<async::env_of_t<Rcvr>>>
struct stop_base<Rcvr, Ops> {
   auto check_stopped() -> bool { return false; }
   auto emplace_stop_cb() -> void {}
   auto clear_stop_cb() -> void {}
   auto reset_cancel_state() -> void {}
   auto mark_queued() -> void {}
   auto mark_cancelled() -> void {}
   [[nodiscard]] auto queued() const -> bool { return true; }
   [[nodiscard]] auto cancelled() const -> bool { return false; }
};

template <stdx::ct_string Name, typename Rcvr>
struct op_state : stop_base<Rcvr, op_state<Name, Rcvr>> {
   [[no_unique_address]] Rcvr rcvr;

   template <stdx::same_as_unqualified<Rcvr> R>
   // NOLINTNEXTLINE(bugprone-forwarding-reference-overload)
   constexpr explicit op_state(R &&r) : rcvr{std::forward<R>(r)} {}

   constexpr auto start() & -> void;

   // Invoked from the stop callback. The whole decision runs in one
   // critical section, atomically with respect to acquires and releases:
   //  - not yet enqueued (start() arms the callback before enqueueing):
   //    mark cancelled and return false — start() observes the mark and
   //    completes stopped without ever enqueueing.
   //  - still in the queue: unlink AND give up the reservation together,
   //    so no release can observe a count that includes us after the queue
   //    no longer does. Returns true iff we dequeued (cancellation won the
   //    race against a hand-off).
   //  - queued but no longer pending: a hand-off already popped us and
   //    run() is making us the holder; cancellation is too late.
   auto stop() -> bool {
      return conc::call_in_critical_section<mutex<Name>>([&]() -> bool {
         if (not this->queued()) {
            this->mark_cancelled();
            return false;
         }
         if (task_.pending) {
            task_.pending = false;
            queue_<Name>.remove(std::addressof(task_));
            --waiters_<Name>;
            return true;
         }
         return false;
      });
   }
   auto complete_stopped() -> void { async::set_stopped(std::move(rcvr)); }

 private:
   struct task_t final : task_base {
      op_state *owner{};
      // Hand-off: we are now the holder. Drop the stop callback (we are no
      // longer cancellable-while-queued) and complete with a value.
      auto run() -> void final {
         owner->clear_stop_cb();
         async::set_value(std::move(owner->rcvr));
      }
   };
   task_t task_{};
};

template <stdx::ct_string Name>
struct acquire_sender {
   using is_sender = void;

   // Completes with a value once the region is acquired, or stopped if a
   // queued waiter is cancelled before it acquires.
   template <typename Env>
   [[nodiscard]] constexpr static auto get_completion_signatures(Env const &)
       -> async::completion_signatures<async::set_value_t(), async::set_stopped_t()> {
      return {};
   }

   template <async::receiver R>
   [[nodiscard]] constexpr auto connect(R &&r) const {
      async::check_connect<acquire_sender, R>();
      return op_state<Name, std::remove_cvref_t<R>>{std::forward<R>(r)};
   }
};

template <stdx::ct_string Name>
struct acquire_fn {
   template <async::channel_tag Tag, typename... Args>
   constexpr auto operator()(Args &&...args) const {
      auto fwd_cmpl = [&] {
         if constexpr (std::same_as<Tag, async::set_value_t>) {
            return async::just(std::forward<Args>(args)...);
         } else if constexpr (std::same_as<Tag, async::set_error_t>) {
            return async::just_error(std::forward<Args>(args)...);
         } else {
            return async::just_stopped();
         }
      };
      return acquire_sender<Name>{} | async::seq(fwd_cmpl());
   }
};

template <stdx::ct_string Name, typename S, typename F>
using acquire_let_sender = async::_let::sender<Name, S, F, async::set_value_t,
                                               async::set_error_t, async::set_stopped_t>;

// Acquire the region, then re-emit the upstream completion unchanged.
template <stdx::ct_string Name>
struct acquire_pipeable {
 private:
   template <async::sender S, stdx::same_as_unqualified<acquire_pipeable> Self>
   friend constexpr auto operator|(S &&s, Self &&) -> async::sender auto {
      return std::forward<S>(s) //
             | async::compose(  //
                   async::_let::pipeable<Name, acquire_fn<Name>, acquire_let_sender>{
                       acquire_fn<Name>{}});
   }
};

template <stdx::ct_string Name, typename Rcvr>
constexpr auto op_state<Name, Rcvr>::start() & -> void {
   task_.owner = this;
   this->reset_cancel_state();
   if (this->check_stopped()) {
      complete_stopped();
      return;
   }
   // Uncontended fast path: take the region without arming cancellation.
   auto const fast = conc::call_in_critical_section<mutex<Name>>([]() -> bool {
      if (waiters_<Name> == 0) {
         ++waiters_<Name>;
         return true;
      }
      return false;
   });
   if (fast) {
      async::set_value(std::move(rcvr));
      return;
   }

   // Contended: arm cancellation *before* becoming visible in the queue.
   // The moment the node is enqueued, a releaser in another context may pop
   // it and complete — and destroy — this op state, so nothing here may
   // touch *this after the enqueue; arming the callback after it (even
   // immediately after) would race with that hand-off. If a stop request
   // fires in the window before the critical section below, the callback
   // finds us not yet queued and just marks us cancelled; we observe the
   // mark under the mutex and complete stopped without ever enqueueing.
   this->emplace_stop_cb();

   enum struct action : std::uint8_t { wait, value, stopped };
   auto const act = conc::call_in_critical_section<mutex<Name>>([&]() -> action {
      if (this->cancelled()) {
         return action::stopped;
      }
      if (waiters_<Name> ++ == 0) {
         // the region was released while we were arming cancellation
         return action::value;
      }
      task_.pending = true;
      queue_<Name>.push_back(std::addressof(task_));
      this->mark_queued();
      return action::wait;
   });
   if (act == action::value) {
      this->clear_stop_cb();
      async::set_value(std::move(rcvr));
   } else if (act == action::stopped) {
      complete_stopped();
   }
   // action::wait — we are a queued waiter: from here a hand-off
   // (task_t::run) or the stop callback (stop) completes us.
}

// Release the region. The decrement and the choice of successor happen in
// ONE critical section: were they split, an acquire interleaving between
// them could fast-path into a region whose hand-off is still pending
// (breaking exclusivity), and a cancellation could leave the count and the
// queue disagreeing (stranding a later waiter forever). Only the completion
// of the chosen successor — arbitrary user code — runs outside.
template <stdx::ct_string Name>
auto do_release() -> void {
   auto *const next = conc::call_in_critical_section<mutex<Name>>([]() -> task_base * {
      if (--waiters_<Name> == 0) {
         return nullptr;
      }
      // nonzero count with the holder gone means queued waiters: the
      // count and the queue only ever change together.
      auto *const t = queue_<Name>.pop_front();
      t->pending = false;
      return t;
   });
   if (next != nullptr) {
      next->run();
   }
}

template <stdx::ct_string Name, typename Rcvr>
struct release_receiver {
   using is_receiver = void;
   [[no_unique_address]] Rcvr rcvr;

   [[nodiscard]] constexpr auto
   query(async::get_env_t) const -> async::forwarding_env<async::env_of_t<Rcvr>> {
      return async::forward_env_of(rcvr);
   }

   template <typename... Args>
   constexpr auto set_value(Args &&...args) && -> void {
      do_release<Name>();
      async::set_value(std::move(rcvr), std::forward<Args>(args)...);
   }
   template <typename... Args>
   constexpr auto set_error(Args &&...args) && -> void {
      do_release<Name>();
      async::set_error(std::move(rcvr), std::forward<Args>(args)...);
   }
   constexpr auto set_stopped() && -> void {
      do_release<Name>();
      async::set_stopped(std::move(rcvr));
   }
};

template <stdx::ct_string Name, async::sender S>
struct release_sender {
   using is_sender = void;
   [[no_unique_address]] S s;

   template <typename Env>
   [[nodiscard]] constexpr static auto
   get_completion_signatures(Env const &) -> async::completion_signatures_of_t<S, Env> {
      return {};
   }

   template <async::receiver R>
   [[nodiscard]] constexpr auto connect(R &&r) && {
      async::check_connect<release_sender &&, R>();
      return async::connect(
          std::move(s), release_receiver<Name, std::remove_cvref_t<R>>{std::forward<R>(r)});
   }

   template <async::receiver R>
      requires async::multishot_sender<S,
                                       async::detail::universal_receiver<async::env_of_t<R>>>
   [[nodiscard]] constexpr auto connect(R &&r) const & {
      async::check_connect<release_sender const &, R>();
      return async::connect(
          s, release_receiver<Name, std::remove_cvref_t<R>>{std::forward<R>(r)});
   }

   [[nodiscard]] constexpr auto query(async::get_env_t) const {
      return async::forward_env_of(s);
   }
};

template <stdx::ct_string Name>
struct release_pipeable {
 private:
   template <async::sender S, stdx::same_as_unqualified<release_pipeable> Self>
   friend constexpr auto operator|(S &&s, Self &&) -> async::sender auto {
      return release_sender<Name, std::remove_cvref_t<S>>{std::forward<S>(s)};
   }
};

// Don't be a knuckle-dragger. These are not for you. Use `within`.
template <stdx::ct_string Name>
constexpr auto acquire() {
   return async::compose(acquire_pipeable<Name>{});
}

template <stdx::ct_string Name>
constexpr auto release() {
   return async::compose(release_pipeable<Name>{});
}

// The `let` handler behind `within`. `tail` is the `mid | release<Name>()`
// stage, where `mid` is `async::seq(s)` (for a sender `s`) or `s` itself (for
// an adaptor `s`).
//
// On whichever channel the upstream `a` completed, acquire the region and —
// *only once acquired* — replay that completion into `tail`. Folding `tail`
// (and thus `release`) inside the acquire's value path is what makes `within`
// cancellable: a queued acquire that is cancelled completes `acquire_sender`
// on the stopped channel, so `async::seq` skips `tail` and the region is
// correctly never released (the cancel already gave up the reservation).
//
// `tail` is moved out on invocation; the handler is single-shot, matching the
// single completion of `a`.
template <stdx::ct_string Name, typename Tail>
struct within_fn {
   [[no_unique_address]] Tail tail;

   template <async::channel_tag Tag, typename... Args>
   constexpr auto operator()(Args &&...args) {
      auto fwd_cmpl = [&] {
         if constexpr (std::same_as<Tag, async::set_value_t>) {
            return async::just(std::forward<Args>(args)...);
         } else if constexpr (std::same_as<Tag, async::set_error_t>) {
            return async::just_error(std::forward<Args>(args)...);
         } else {
            return async::just_stopped();
         }
      };
      return acquire_sender<Name>{} | async::seq(fwd_cmpl() | std::move(tail));
   }
};

// The pipeable behind `within`: `a | within_pipeable` becomes
// `a | let<value,error,stopped>(within_fn{tail})`.
template <stdx::ct_string Name, typename Tail>
struct within_pipeable {
   [[no_unique_address]] Tail tail;

 private:
   template <async::sender S, stdx::same_as_unqualified<within_pipeable> Self>
   friend constexpr auto operator|(S &&s, Self &&self) -> async::sender auto {
      return                                                 //
          std::forward<S>(s)                                 //
          | async::compose(                                  //
                async::_let::pipeable<Name,                  //
                                      within_fn<Name, Tail>, //
                                      acquire_let_sender>{
                    within_fn<Name, Tail>{std::forward<Self>(self).tail} //
                });
   }
};

} // namespace detail

// Run `s` inside the exclusive region `Name`.
//
//     a | region::within<Name>(s) | b
//
// Returns a composed adaptor. If `s` is a sender it is sequenced after the
// acquire (running only on an upstream value); if `s` is itself a composed
// adaptor the upstream completion is threaded straight into it. In both
// cases the region is acquired before `s` and released after, regardless of
// the upstream completion channel. See the file header for the full "as-if"
// decomposition.
//
// While a `within` is queued waiting for the region, a stop request on the
// receiver cancels it: it leaves the queue and completes stopped without ever
// acquiring (and therefore without releasing).
//
template <stdx::ct_string Name, typename S>
constexpr auto within(S &&s) {
   auto make = [](auto mid) {
      auto tail = std::move(mid) | detail::release<Name>();
      return async::compose(detail::within_pipeable<Name, decltype(tail)>{std::move(tail)});
   };
   if constexpr (async::sender<std::remove_cvref_t<S>>) {
      return make(async::seq(std::forward<S>(s)));
   } else {
      return make(std::forward<S>(s));
   }
}

} // namespace region

template <stdx::ct_string Name, typename Rcvr>
struct async::debug::context_for<region::detail::op_state<Name, Rcvr>> {
   using tag = region::sender_t;
   constexpr static auto name = Name;
   using type = region::detail::op_state<Name, Rcvr>;
   using children = stdx::type_list<>;
};
