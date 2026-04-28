// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// A `region` is a named exclusive region for senders: only one sender
// may execute inside the region at a time. Primitives `acquire<Name>()`
// and `release<Name>()` are available, but don't be a low-level user —
// just call `within<Name>(s)` and pass the sender.
//
// Uses the trigger_manager's intrusive task list for
// zero-allocation FIFO queuing of waiters.
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
#include <async/let_value.hpp>
#include <async/schedulers/trigger_manager.hpp>
#include <async/sequence.hpp>
#include <async/type_traits.hpp>

#include <stdx/concepts.hpp>
#include <stdx/ct_string.hpp>

#include <conc/concurrency.hpp>

#include <type_traits>
#include <utility>

namespace region {

struct sender_t;

namespace detail {

template <stdx::ct_string Name> struct mutex;
template <stdx::ct_string Name> inline int waiters_{};

template <stdx::ct_string Name, typename Rcvr> struct op_state {
    [[no_unique_address]] Rcvr rcvr;

    template <stdx::same_as_unqualified<Rcvr> R>
    // NOLINTNEXTLINE(bugprone-forwarding-reference-overload)
    constexpr explicit op_state(R &&r) : rcvr{std::forward<R>(r)} {}

    constexpr auto start() & -> void;

  private:
    struct task_t final : async::trigger_task<> {
        op_state *owner{};
        auto run() -> void final { async::set_value(std::move(owner->rcvr)); }
    };
    task_t task_{};
};

template <stdx::ct_string Name> struct acquire_sender {
    using is_sender = void;

    template <typename Env>
    [[nodiscard]] constexpr static auto get_completion_signatures(Env const &)
        -> async::completion_signatures<async::set_value_t()> {
        return {};
    }

    template <async::receiver R>
    [[nodiscard]] constexpr auto connect(R &&r) const {
        async::check_connect<acquire_sender, R>();
        return op_state<Name, std::remove_cvref_t<R>>{std::forward<R>(r)};
    }
};

template <stdx::ct_string Name> struct acquire_pipeable {
  private:
    template <async::sender S, stdx::same_as_unqualified<acquire_pipeable> Self>
    friend constexpr auto operator|(S &&s, Self &&) -> async::sender auto {
        return                                                            //
            std::forward<S>(s)                                            //
            | async::let_value([](auto &&...args) {                       //
                  return                                                  //
                      acquire_sender<Name>{}                              //
                      | async::seq(                                       //
                            async::just(                                  //
                                std::forward<decltype(args)...>(args)...) //
                            )                                             //
                      ;
              });
    }
};

template <stdx::ct_string Name, typename Rcvr>
constexpr auto op_state<Name, Rcvr>::start() & -> void {
    task_.owner = this;
    auto const fast =
        conc::call_in_critical_section<mutex<Name>>([this]() -> bool {
            if (waiters_<Name> ++ == 0) {
                return true;
            }
            async::triggers<stdx::cts_t<Name>>.enqueue(task_);
            return false;
        });

    if (fast) {
        async::set_value(std::move(rcvr));
    }
}

template <stdx::ct_string Name> auto do_release() -> void {
    auto const has_waiter = conc::call_in_critical_section<mutex<Name>>(
        []() -> bool { return (--waiters_<Name> != 0); });
    if (has_waiter) {
        async::run_one_trigger<Name>();
    }
}

template <stdx::ct_string Name, typename Rcvr> struct release_receiver {
    using is_receiver = void;
    [[no_unique_address]] Rcvr rcvr;

    [[nodiscard]] constexpr auto query(async::get_env_t) const
        -> async::forwarding_env<async::env_of_t<Rcvr>> {
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

template <stdx::ct_string Name, async::sender S> struct release_sender {
    using is_sender = void;
    [[no_unique_address]] S s;

    template <typename Env>
    [[nodiscard]] constexpr static auto get_completion_signatures(Env const &)
        -> async::completion_signatures_of_t<S, Env> {
        return {};
    }

    template <async::receiver R>
    [[nodiscard]] constexpr auto connect(R &&r) && {
        async::check_connect<release_sender &&, R>();
        return async::connect(
            std::move(s),
            release_receiver<Name, std::remove_cvref_t<R>>{std::forward<R>(r)});
    }

    template <async::receiver R>
        requires async::multishot_sender<
            S, async::detail::universal_receiver<async::env_of_t<R>>>
    [[nodiscard]] constexpr auto connect(R &&r) const & {
        async::check_connect<release_sender const &, R>();
        return async::connect(
            s,
            release_receiver<Name, std::remove_cvref_t<R>>{std::forward<R>(r)});
    }

    [[nodiscard]] constexpr auto query(async::get_env_t) const {
        return async::forward_env_of(s);
    }
};

template <stdx::ct_string Name> struct release_pipeable {
  private:
    template <async::sender S, stdx::same_as_unqualified<release_pipeable> Self>
    friend constexpr auto operator|(S &&s, Self &&) -> async::sender auto {
        return release_sender<Name, std::remove_cvref_t<S>>{std::forward<S>(s)};
    }
};

} // namespace detail

template <stdx::ct_string Name> constexpr auto acquire() {
    return async::compose(detail::acquire_pipeable<Name>{});
}

template <stdx::ct_string Name> constexpr auto release() {
    return async::compose(detail::release_pipeable<Name>{});
}

template <stdx::ct_string Name, async::sender S>
constexpr auto within(S &&s) -> async::sender auto {
    return                                                                //
        detail::acquire_sender<Name>{}                                    //
        | async::let_value([s = std::forward<S>(s)](auto &&...) mutable { //
              return std::move(s);                                        //
          })                                                              //
        | release<Name>()                                                 //
        ;
}

} // namespace region

template <stdx::ct_string Name, typename Rcvr>
struct async::debug::context_for<region::detail::op_state<Name, Rcvr>> {
    using tag = region::sender_t;
    constexpr static auto name = Name;
    using type = region::detail::op_state<Name, Rcvr>;
    using children = stdx::type_list<>;
};
