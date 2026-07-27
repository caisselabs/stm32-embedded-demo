// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
#include <async/concepts.hpp>
#include <async/connect.hpp>
#include <async/env.hpp>
#include <async/just.hpp>
#include <async/just_result_of.hpp>
#include <async/let_error.hpp>
#include <async/schedulers/trigger_manager.hpp>
#include <async/schedulers/trigger_scheduler.hpp>
#include <async/sequence.hpp>
#include <async/start.hpp>
#include <async/stop_token.hpp>
#include <async/sync_wait.hpp>
#include <async/then.hpp>

#include <stdx/ct_string.hpp>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <shared/region.hpp>

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------
namespace {

struct universal_receiver {
    using is_receiver = void;
    constexpr auto set_value(auto &&...) const && -> void {}
    constexpr auto set_error(auto &&...) const && -> void {}
    constexpr auto set_stopped() const && -> void {}
};

template <typename F> struct receiver {
    using is_receiver = void;
    constexpr auto set_value(auto &&...args) const && -> void {
        f(std::forward<decltype(args)>(args)...);
    }
    constexpr auto set_error(auto &&...) const && -> void {}
    constexpr auto set_stopped() const && -> void {}
    F f;
};
template <typename F> receiver(F) -> receiver<F>;

template <typename F> struct error_receiver {
    using is_receiver = void;
    constexpr auto set_value(auto &&...) const && -> void {}
    constexpr auto set_error(auto &&...args) const && -> void {
        f(std::forward<decltype(args)>(args)...);
    }
    constexpr auto set_stopped() const && -> void {}
    F f;
};
template <typename F> error_receiver(F) -> error_receiver<F>;

template <typename F> struct stop_receiver {
    using is_receiver = void;
    constexpr auto set_value(auto &&...) const && -> void {}
    constexpr auto set_error(auto &&...) const && -> void {}
    constexpr auto set_stopped() const && -> void { f(); }
    F f;
};
template <typename F> stop_receiver(F) -> stop_receiver<F>;

// A receiver that carries a real stop token in its env, so cancellation can
// be driven from a test via the inplace_stop_source. `f` runs on set_stopped.
template <typename F> struct cancellable_receiver {
    using is_receiver = void;
    async::inplace_stop_token token;
    F f;
    constexpr auto set_value(auto &&...) const && -> void {}
    constexpr auto set_error(auto &&...) const && -> void {}
    constexpr auto set_stopped() const && -> void { f(); }
    constexpr auto query(async::get_env_t) const {
        return async::env{async::prop{async::get_stop_token_t{}, token}};
    }
};
template <typename F>
cancellable_receiver(async::inplace_stop_token, F) -> cancellable_receiver<F>;

template <typename T>
constexpr auto unique_name =
    stdx::ct_string<stdx::type_as_string<T>().size() + 1>{
        stdx::type_as_string<T>()};

} // namespace

// ===================================================================
// acquire primitive (region::detail)
// ===================================================================

TEMPLATE_TEST_CASE("acquire adaptor completes immediately when not busy",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    int value = 0;
    auto s = async::just_result_of([&] { value = 42; });
    CHECK((s | region::detail::acquire<name>() | async::sync_wait()).has_value());
    CHECK(value == 42);
    CHECK(region::detail::waiters_<name> > 0);
    CHECK(async::triggers<stdx::cts_t<name>>.empty());
}

TEMPLATE_TEST_CASE("acquire adaptor enqueues when busy", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    bool past_acquire = false;
    auto op = async::connect(async::just() | region::detail::acquire<name>() |
                                 async::then([&] { past_acquire = true; }),
                             universal_receiver{});
    async::start(op);
    CHECK_FALSE(past_acquire);
    CHECK_FALSE(async::triggers<stdx::cts_t<name>>.empty());
}

TEMPLATE_TEST_CASE("acquire adaptor forwards upstream values", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    int got = 0;
    CHECK((async::just(42) | region::detail::acquire<name>() |
           async::then([&](int v) { got = v; }) | async::sync_wait())
              .has_value());
    CHECK(got == 42);
}

TEMPLATE_TEST_CASE("acquire adaptor forwards upstream values when queued",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    int got = 0;
    auto op = async::connect(async::just(99) | region::detail::acquire<name>() |
                                 async::then([&](int v) { got = v; }),
                             universal_receiver{});
    async::start(op);
    CHECK(got == 0);

    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(got == 99);
}

TEMPLATE_TEST_CASE("acquire adaptor acquires and forwards upstream error",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    int err = 0;
    auto op =
        async::connect(async::just_error(5) | region::detail::acquire<name>(),
                       error_receiver{[&](int e) { err = e; }});
    async::start(op);
    CHECK(err == 5);
    // acquire ran even though the upstream errored
    CHECK(region::detail::waiters_<name> > 0);

    // clean up: release the region the acquire took
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("acquire adaptor acquires and forwards upstream stop",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    bool stopped = false;
    auto op =
        async::connect(async::just_stopped() | region::detail::acquire<name>(),
                       stop_receiver{[&] { stopped = true; }});
    async::start(op);
    CHECK(stopped);
    // acquire ran even though the upstream stopped
    CHECK(region::detail::waiters_<name> > 0);

    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("acquire adaptor enqueues on error channel when busy",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    bool woke = false;
    auto op =
        async::connect(async::just_error(3) | region::detail::acquire<name>(),
                       error_receiver{[&](int) { woke = true; }});
    async::start(op);
    // suspended waiting for the region even on the error channel
    CHECK_FALSE(woke);
    CHECK_FALSE(async::triggers<stdx::cts_t<name>>.empty());

    // hand off: the queued acquire wakes and re-emits the error
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(woke);
    CHECK(region::detail::waiters_<name> > 0);

    // clean up: drain the region the woken acquire now holds
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(region::detail::waiters_<name> == 0);
}

// ===================================================================
// release primitive (region::detail)
// ===================================================================

TEMPLATE_TEST_CASE("release adaptor clears busy when no waiters", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("release adaptor hands off to queued waiter", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    // Enqueue a waiter via the acquire sender (connect + start)
    bool acquired = false;
    auto wait_op = async::connect(async::just() | region::detail::acquire<name>(),
                                  receiver{[&] { acquired = true; }});
    async::start(wait_op);
    CHECK_FALSE(acquired);

    // Release hands off
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(acquired);
    CHECK(region::detail::waiters_<name> > 0);
}

TEMPLATE_TEST_CASE("release adaptor forwards upstream values", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    int got = 0;
    CHECK((async::just(42) | region::detail::release<name>() |
           async::then([&](int v) { got = v; }) | async::sync_wait())
              .has_value());
    CHECK(got == 42);
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("release adaptor forwards upstream values with waiter",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    bool waiter_woke = false;
    auto wait_op = async::connect(async::just() | region::detail::acquire<name>(),
                                  receiver{[&] { waiter_woke = true; }});
    async::start(wait_op);

    int got = 0;
    CHECK((async::just(77) | region::detail::release<name>() |
           async::then([&](int v) { got = v; }) | async::sync_wait())
              .has_value());
    CHECK(got == 77);
    CHECK(waiter_woke);

    // Clean up: the waiter holds the bus; release so state is reset.
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("release adaptor fires on error channel and forwards error",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    int err = 0;
    auto op =
        async::connect(async::just_error(123) | region::detail::release<name>(),
                       error_receiver{[&](int e) { err = e; }});
    async::start(op);
    CHECK(err == 123);
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("release adaptor fires on error channel and wakes waiter",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    bool waiter_woke = false;
    auto wait_op = async::connect(async::just() | region::detail::acquire<name>(),
                                  receiver{[&] { waiter_woke = true; }});
    async::start(wait_op);

    auto op =
        async::connect(async::just_error(0) | region::detail::release<name>(),
                       universal_receiver{});
    async::start(op);
    CHECK(waiter_woke);

    // Clean up: drain the waiter that now holds the bus.
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("release adaptor fires on stop channel and forwards stop",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    bool stopped = false;
    auto op =
        async::connect(async::just_stopped() | region::detail::release<name>(),
                       stop_receiver{[&] { stopped = true; }});
    async::start(op);
    CHECK(stopped);
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("release adaptor fires on stop channel and wakes waiter",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    bool waiter_woke = false;
    auto wait_op = async::connect(async::just() | region::detail::acquire<name>(),
                                  receiver{[&] { waiter_woke = true; }});
    async::start(wait_op);

    auto op =
        async::connect(async::just_stopped() | region::detail::release<name>(),
                       universal_receiver{});
    async::start(op);
    CHECK(waiter_woke);

    // Clean up: drain the waiter that now holds the bus.
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(region::detail::waiters_<name> == 0);
}

// ===================================================================
// FIFO ordering
// ===================================================================

TEMPLATE_TEST_CASE("release wakes waiters in FIFO order", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    std::vector<int> order;

    auto op1 = async::connect(async::just() | region::detail::acquire<name>(),
                              receiver{[&] { order.push_back(1); }});
    async::start(op1);

    auto op2 = async::connect(async::just() | region::detail::acquire<name>(),
                              receiver{[&] { order.push_back(2); }});
    async::start(op2);

    auto op3 = async::connect(async::just() | region::detail::acquire<name>(),
                              receiver{[&] { order.push_back(3); }});
    async::start(op3);

    CHECK(order.empty());

    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());

    REQUIRE(order.size() == 3);
    CHECK(order[0] == 1);
    CHECK(order[1] == 2);
    CHECK(order[2] == 3);
}

// ===================================================================
// within adaptor
// ===================================================================

TEMPLATE_TEST_CASE("within runs sender between acquire and release", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    int value = 0;
    auto s = async::just_result_of([&] { value = 42; });
    CHECK((async::just() | region::within<name>(s) | async::sync_wait())
              .has_value());
    CHECK(value == 42);
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("within forwards the sender's value downstream", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    int got = 0;
    CHECK((async::just() | region::within<name>(async::just(7)) |
           async::then([&](int v) { got = v; }) | async::sync_wait())
              .has_value());
    CHECK(got == 7);
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("within serializes sequential operations", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    std::vector<int> trace;

    auto s1 = async::just_result_of([&] { trace.push_back(1); });
    CHECK((async::just() | region::within<name>(s1) | async::sync_wait())
              .has_value());

    auto s2 = async::just_result_of([&] { trace.push_back(2); });
    CHECK((async::just() | region::within<name>(s2) | async::sync_wait())
              .has_value());

    REQUIRE(trace.size() == 2);
    CHECK(trace[0] == 1);
    CHECK(trace[1] == 2);
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("within queues when busy", "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    std::vector<int> trace;

    // First: acquire + run sender, but don't release yet
    auto s1 = async::just_result_of([&] { trace.push_back(1); });
    auto first = s1 | region::detail::acquire<name>();
    auto op1 = async::connect(first, universal_receiver{});
    async::start(op1);
    CHECK(trace == std::vector{1});
    CHECK(region::detail::waiters_<name> > 0);

    // Second: full within — blocked on acquire
    bool second_ran = false;
    auto s2 = async::just_result_of([&] {
        second_ran = true;
        trace.push_back(2);
    });
    auto op2 = async::connect(async::just() | region::within<name>(s2),
                              universal_receiver{});
    async::start(op2);
    CHECK_FALSE(second_ran);

    // Release the first — second runs and auto-releases
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(second_ran);
    REQUIRE(trace.size() == 2);
    CHECK(trace[0] == 1);
    CHECK(trace[1] == 2);
    CHECK(region::detail::waiters_<name> == 0);
}

// ===================================================================
// within — sender argument gated by the upstream completion
// ===================================================================

TEMPLATE_TEST_CASE("within skips a sender arg on upstream error but still "
                   "cycles the region",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    bool s_ran = false;
    auto s = async::just_result_of([&] { s_ran = true; });

    int err = 0;
    auto op = async::connect(async::just_error(11) | region::within<name>(s),
                             error_receiver{[&](int e) { err = e; }});
    async::start(op);

    // sender arg was skipped (upstream was not a value) ...
    CHECK_FALSE(s_ran);
    // ... the error propagated unchanged ...
    CHECK(err == 11);
    // ... and acquire/release both ran, leaving the region free.
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("within skips a sender arg on upstream stop but still "
                   "cycles the region",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    bool s_ran = false;
    auto s = async::just_result_of([&] { s_ran = true; });

    bool stopped = false;
    auto op = async::connect(async::just_stopped() | region::within<name>(s),
                             stop_receiver{[&] { stopped = true; }});
    async::start(op);

    CHECK_FALSE(s_ran);
    CHECK(stopped);
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("within acquires and releases around a queued error "
                   "(sender arg skipped)",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    // Hold the region so within must queue.
    auto hold = async::just() | region::detail::acquire<name>();
    auto hold_op = async::connect(hold, universal_receiver{});
    async::start(hold_op);
    CHECK(region::detail::waiters_<name> > 0);

    bool s_ran = false;
    auto s = async::just_result_of([&] { s_ran = true; });

    int err = 0;
    auto op = async::connect(async::just_error(11) | region::within<name>(s),
                             error_receiver{[&](int e) { err = e; }});
    async::start(op);

    // Blocked on acquire even though the upstream is an error.
    CHECK(err == 0);
    CHECK_FALSE(s_ran);

    // Hand off: within acquires, skips the sender (error), releases.
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK_FALSE(s_ran);
    CHECK(err == 11);
    CHECK(region::detail::waiters_<name> == 0);
}

// ===================================================================
// within — adaptor argument sees every upstream completion
// ===================================================================

TEMPLATE_TEST_CASE("within threads the upstream value into an adaptor arg",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    // s is a composed adaptor, not a sender.
    auto s = async::then([](int v) { return v + 1; });

    int got = 0;
    CHECK((async::just(41) | region::within<name>(s) |
           async::then([&](int v) { got = v; }) | async::sync_wait())
              .has_value());
    CHECK(got == 42);
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("within threads an upstream error into an adaptor arg",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    // Adaptor arg observes the error channel (a sender arg would be skipped).
    int adaptor_saw = 0;
    auto s = async::let_error([&](int e) {
        adaptor_saw = e;
        return async::just_error(e);
    });

    int err = 0;
    auto op = async::connect(async::just_error(9) | region::within<name>(s),
                             error_receiver{[&](int e) { err = e; }});
    async::start(op);

    CHECK(adaptor_saw == 9);
    CHECK(err == 9);
    CHECK(region::detail::waiters_<name> == 0);
}

// ===================================================================
// Edge cases
// ===================================================================

TEMPLATE_TEST_CASE("second acquire blocks while first holds bus", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    std::vector<int> trace;

    // First acquire succeeds immediately
    auto op1 = async::connect(async::just_result_of([&] {
                                  trace.push_back(1);
                              }) | region::detail::acquire<name>(),
                              universal_receiver{});
    async::start(op1);
    CHECK(trace == std::vector{1});

    // Second acquire blocks
    bool second_acquired = false;
    auto op2 = async::connect(async::just() | region::detail::acquire<name>() |
                                  async::then([&] { second_acquired = true; }),
                              universal_receiver{});
    async::start(op2);
    CHECK_FALSE(second_acquired);

    // Release unblocks the second
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(second_acquired);
    CHECK(region::detail::waiters_<name> > 0);

    // Clean up
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(region::detail::waiters_<name> == 0);
}

// ===================================================================
// within — cancellation while queued
// ===================================================================

TEMPLATE_TEST_CASE("within cancels while queued and never acquires", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    // Hold the region so within must queue.
    auto hold_op = async::connect(
        async::just() | region::detail::acquire<name>(), universal_receiver{});
    async::start(hold_op);
    CHECK(region::detail::waiters_<name> == 1);

    async::inplace_stop_source src;
    bool body_ran = false;
    bool stopped = false;
    auto s = async::just_result_of([&] { body_ran = true; });
    auto op = async::connect(
        async::just() | region::within<name>(s),
        cancellable_receiver{src.get_token(), [&] { stopped = true; }});
    async::start(op);

    // Queued behind the holder: nothing has run.
    CHECK_FALSE(body_ran);
    CHECK_FALSE(stopped);
    CHECK(region::detail::waiters_<name> == 2);

    // Cancel: within leaves the queue and completes stopped without acquiring.
    src.request_stop();
    CHECK(stopped);
    CHECK_FALSE(body_ran);
    // The holder is undisturbed; the cancelled waiter gave up its reservation.
    CHECK(region::detail::waiters_<name> == 1);

    // Releasing the holder frees the region — no phantom waiter to wake.
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("cancelling a queued within leaves other waiters intact",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    // Hold the region.
    auto hold_op = async::connect(
        async::just() | region::detail::acquire<name>(), universal_receiver{});
    async::start(hold_op);

    // Y: a cancellable within, first in the FIFO.
    async::inplace_stop_source src;
    bool y_ran = false;
    bool y_stopped = false;
    auto y_s = async::just_result_of([&] { y_ran = true; });
    auto y_op = async::connect(
        async::just() | region::within<name>(y_s),
        cancellable_receiver{src.get_token(), [&] { y_stopped = true; }});
    async::start(y_op);

    // Z: a plain waiter behind Y.
    bool z_woke = false;
    auto z_op = async::connect(async::just() | region::detail::acquire<name>(),
                               receiver{[&] { z_woke = true; }});
    async::start(z_op);
    CHECK(region::detail::waiters_<name> == 3);

    // Cancel Y — Z must be untouched and still queued.
    src.request_stop();
    CHECK(y_stopped);
    CHECK_FALSE(y_ran);
    CHECK_FALSE(z_woke);
    CHECK(region::detail::waiters_<name> == 2);

    // Releasing the holder hands off to Z (not the cancelled Y).
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(z_woke);
    CHECK_FALSE(y_ran);
    CHECK(region::detail::waiters_<name> > 0);

    // Clean up: drain Z's hold.
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("within completes stopped if stop is already requested",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    // Region is free, but the token is already stopped.
    async::inplace_stop_source src;
    src.request_stop();

    bool body_ran = false;
    bool stopped = false;
    auto s = async::just_result_of([&] { body_ran = true; });
    auto op = async::connect(
        async::just() | region::within<name>(s),
        cancellable_receiver{src.get_token(), [&] { stopped = true; }});
    async::start(op);

    // Never acquired, never ran the body, region untouched.
    CHECK(stopped);
    CHECK_FALSE(body_ran);
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("a hand-off beats a later cancel", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    // Hold the region so within queues.
    auto hold_op = async::connect(
        async::just() | region::detail::acquire<name>(), universal_receiver{});
    async::start(hold_op);

    async::inplace_stop_source src;
    bool body_ran = false;
    bool stopped = false;
    auto s = async::just_result_of([&] { body_ran = true; });
    auto op = async::connect(
        async::just() | region::within<name>(s),
        cancellable_receiver{src.get_token(), [&] { stopped = true; }});
    async::start(op);
    CHECK(region::detail::waiters_<name> == 2);

    // Release the holder first: within is handed the region, runs, releases.
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(body_ran);
    CHECK_FALSE(stopped);
    CHECK(region::detail::waiters_<name> == 0);

    // A stop now arrives too late — it must be an inert no-op (the stop
    // callback was disarmed on hand-off), not a double completion.
    src.request_stop();
    CHECK(body_ran);
    CHECK_FALSE(stopped);
    CHECK(region::detail::waiters_<name> == 0);
}

// ===================================================================
// within — cancellation while holding the region
//
// The cases above cancel a `within` that is still queued: it never
// acquires, so it must never release. These cancel one that is *inside*
// the region — acquired, body in flight, not yet released. Region code
// sees no stop request here (the acquire's own stop callback is either
// never armed, on the fast path, or dropped on hand-off): the stop is the
// body's to handle, and it must come back out on the stopped channel so
// that `release` runs and the region is handed on. A leaked reservation
// here wedges the region permanently.
//
// The body is a `trigger_scheduler` schedule: it parks until the test
// either fires its trigger or cancels it.
// ===================================================================

TEMPLATE_TEST_CASE("within cancels mid-region and still releases", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    async::inplace_stop_source src;
    bool body_ran = false;
    int stop_count{};
    auto s = async::trigger_scheduler<"mid-cancel">::schedule() |
             async::then([&] { body_ran = true; });
    auto op = async::connect(
        async::just() | region::within<name>(s),
        cancellable_receiver{src.get_token(), [&] { ++stop_count; }});
    async::start(op);

    // The region was free, so within took it immediately; the body is in
    // flight, parked on its trigger.
    CHECK(region::detail::waiters_<name> == 1);
    CHECK_FALSE(body_ran);
    CHECK(stop_count == 0);

    // Cancel from inside the region: the body completes stopped and the
    // stop travels out through release, which must free the region.
    src.request_stop();
    CHECK(stop_count == 1);
    CHECK_FALSE(body_ran);
    CHECK(region::detail::waiters_<name> == 0);

    // The body really left its trigger queue — firing it now does nothing.
    async::run_triggers<"mid-cancel">();
    CHECK_FALSE(body_ran);
    CHECK(stop_count == 1);

    // The region is genuinely free, not merely counted free. Started rather
    // than sync_wait'd on purpose: a wedged region must fail this check, not
    // block the suite forever.
    bool next_ran = false;
    auto next_op = async::connect(
        async::just() | region::within<name>(
                            async::just_result_of([&] { next_ran = true; })),
        universal_receiver{});
    async::start(next_op);
    CHECK(next_ran);
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("cancelling mid-region hands off to the next waiter",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    // The within acquires immediately and parks on its body trigger.
    async::inplace_stop_source src;
    bool body_ran = false;
    int stop_count{};
    auto s = async::trigger_scheduler<"mid-cancel-handoff">::schedule() |
             async::then([&] { body_ran = true; });
    auto op = async::connect(
        async::just() | region::within<name>(s),
        cancellable_receiver{src.get_token(), [&] { ++stop_count; }});
    async::start(op);
    CHECK(region::detail::waiters_<name> == 1);

    // Z queues behind it.
    bool z_woke = false;
    auto z_op = async::connect(async::just() | region::detail::acquire<name>(),
                               receiver{[&] { z_woke = true; }});
    async::start(z_op);
    CHECK(region::detail::waiters_<name> == 2);
    CHECK_FALSE(z_woke);

    // Cancel mid-body: the release on the stopped channel must wake Z.
    src.request_stop();
    CHECK(stop_count == 1);
    CHECK_FALSE(body_ran);
    CHECK(z_woke);
    CHECK(region::detail::waiters_<name> == 1);

    // Clean up: drain Z's hold.
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("within cancels mid-region after being handed the region",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    // Hold the region so within queues first.
    auto hold_op = async::connect(
        async::just() | region::detail::acquire<name>(), universal_receiver{});
    async::start(hold_op);

    async::inplace_stop_source src;
    bool body_ran = false;
    int stop_count{};
    auto s = async::trigger_scheduler<"mid-cancel-queued">::schedule() |
             async::then([&] { body_ran = true; });
    auto op = async::connect(
        async::just() | region::within<name>(s),
        cancellable_receiver{src.get_token(), [&] { ++stop_count; }});
    async::start(op);
    CHECK(region::detail::waiters_<name> == 2);
    CHECK_FALSE(body_ran);

    // Hand off: within acquires and its body starts, then parks.
    CHECK((async::just() | region::detail::release<name>() | async::sync_wait())
              .has_value());
    CHECK(region::detail::waiters_<name> == 1);
    CHECK_FALSE(body_ran);
    CHECK(stop_count == 0);

    // Now cancel. The acquire's stop callback was dropped on hand-off, so
    // this is the body's stop alone — exactly one completion, and the
    // region is released on the way out.
    src.request_stop();
    CHECK(stop_count == 1);
    CHECK_FALSE(body_ran);
    CHECK(region::detail::waiters_<name> == 0);

    async::run_triggers<"mid-cancel-queued">();
    CHECK_FALSE(body_ran);
    CHECK(stop_count == 1);
}

TEMPLATE_TEST_CASE("a mid-region body that completes beats a later cancel",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    async::inplace_stop_source src;
    bool body_ran = false;
    int stop_count{};
    auto s = async::trigger_scheduler<"mid-cancel-late">::schedule() |
             async::then([&] { body_ran = true; });
    auto op = async::connect(
        async::just() | region::within<name>(s),
        cancellable_receiver{src.get_token(), [&] { ++stop_count; }});
    async::start(op);
    CHECK(region::detail::waiters_<name> == 1);

    // Fire the trigger first: the body runs and within releases normally.
    async::run_triggers<"mid-cancel-late">();
    CHECK(body_ran);
    CHECK(stop_count == 0);
    CHECK(region::detail::waiters_<name> == 0);

    // A stop arriving after the fact is inert — no second completion, and
    // no second release driving the count negative.
    src.request_stop();
    CHECK(stop_count == 0);
    CHECK(region::detail::waiters_<name> == 0);
}
