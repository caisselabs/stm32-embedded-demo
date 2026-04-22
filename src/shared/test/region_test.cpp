// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
#include <async/concepts.hpp>
#include <async/connect.hpp>
#include <async/just.hpp>
#include <async/just_result_of.hpp>
#include <async/schedulers/trigger_manager.hpp>
#include <async/sequence.hpp>
#include <async/start.hpp>
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

template <typename T>
constexpr auto unique_name =
    stdx::ct_string<stdx::type_as_string<T>().size() + 1>{
        stdx::type_as_string<T>()};

} // namespace

// ===================================================================
// acquire adaptor
// ===================================================================

TEMPLATE_TEST_CASE("acquire adaptor completes immediately when not busy",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    int value = 0;
    auto s = async::just_result_of([&] { value = 42; });
    CHECK((s | region::acquire<name>() | async::sync_wait()).has_value());
    CHECK(value == 42);
    CHECK(region::detail::waiters_<name> > 0);
    CHECK(async::triggers<stdx::cts_t<name>>.empty());
}

TEMPLATE_TEST_CASE("acquire adaptor enqueues when busy", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    bool past_acquire = false;
    auto op = async::connect(async::just() | region::acquire<name>() |
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
    CHECK((async::just(42) | region::acquire<name>() |
           async::then([&](int v) { got = v; }) | async::sync_wait())
              .has_value());
    CHECK(got == 42);
}

TEMPLATE_TEST_CASE("acquire adaptor forwards upstream values when queued",
                   "[region]", decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    int got = 0;
    auto op = async::connect(async::just(99) | region::acquire<name>() |
                                 async::then([&](int v) { got = v; }),
                             universal_receiver{});
    async::start(op);
    CHECK(got == 0);

    CHECK((async::just() | region::release<name>() | async::sync_wait())
              .has_value());
    CHECK(got == 99);
}

// ===================================================================
// release adaptor
// ===================================================================

TEMPLATE_TEST_CASE("release adaptor clears busy when no waiters", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    CHECK((async::just() | region::release<name>() | async::sync_wait())
              .has_value());
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("release adaptor hands off to queued waiter", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    // Enqueue a waiter via the acquire sender (connect + start)
    bool acquired = false;
    auto wait_op = async::connect(async::just() | region::acquire<name>(),
                                  receiver{[&] { acquired = true; }});
    async::start(wait_op);
    CHECK_FALSE(acquired);

    // Release hands off
    CHECK((async::just() | region::release<name>() | async::sync_wait())
              .has_value());
    CHECK(acquired);
    CHECK(region::detail::waiters_<name> > 0);
}

TEMPLATE_TEST_CASE("release adaptor forwards upstream values", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 1;

    int got = 0;
    CHECK((async::just(42) | region::release<name>() |
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
    auto wait_op = async::connect(async::just() | region::acquire<name>(),
                                  receiver{[&] { waiter_woke = true; }});
    async::start(wait_op);

    int got = 0;
    CHECK((async::just(77) | region::release<name>() |
           async::then([&](int v) { got = v; }) | async::sync_wait())
              .has_value());
    CHECK(got == 77);
    CHECK(waiter_woke);

    // Clean up: the waiter holds the bus; release so state is reset.
    CHECK((async::just() | region::release<name>() | async::sync_wait())
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

    auto op1 = async::connect(async::just() | region::acquire<name>(),
                              receiver{[&] { order.push_back(1); }});
    async::start(op1);

    auto op2 = async::connect(async::just() | region::acquire<name>(),
                              receiver{[&] { order.push_back(2); }});
    async::start(op2);

    auto op3 = async::connect(async::just() | region::acquire<name>(),
                              receiver{[&] { order.push_back(3); }});
    async::start(op3);

    CHECK(order.empty());

    CHECK((async::just() | region::release<name>() | async::sync_wait())
              .has_value());
    CHECK((async::just() | region::release<name>() | async::sync_wait())
              .has_value());
    CHECK((async::just() | region::release<name>() | async::sync_wait())
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
    CHECK((region::within<name>(s) | async::sync_wait()).has_value());
    CHECK(value == 42);
    CHECK(region::detail::waiters_<name> == 0);
}

TEMPLATE_TEST_CASE("within serializes sequential operations", "[region]",
                   decltype([] {})) {
    constexpr auto name = unique_name<TestType>;
    region::detail::waiters_<name> = 0;

    std::vector<int> trace;

    auto s1 = async::just_result_of([&] { trace.push_back(1); });
    CHECK((region::within<name>(s1) | async::sync_wait()).has_value());

    auto s2 = async::just_result_of([&] { trace.push_back(2); });
    CHECK((region::within<name>(s2) | async::sync_wait()).has_value());

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
    auto first = s1 | region::acquire<name>();
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
    auto op2 = async::connect(region::within<name>(s2), universal_receiver{});
    async::start(op2);
    CHECK_FALSE(second_ran);

    // Release the first — second runs and auto-releases
    CHECK((async::just() | region::release<name>() | async::sync_wait())
              .has_value());
    CHECK(second_ran);
    REQUIRE(trace.size() == 2);
    CHECK(trace[0] == 1);
    CHECK(trace[1] == 2);
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
                              }) | region::acquire<name>(),
                              universal_receiver{});
    async::start(op1);
    CHECK(trace == std::vector{1});

    // Second acquire blocks
    bool second_acquired = false;
    auto op2 = async::connect(async::just() | region::acquire<name>() |
                                  async::then([&] { second_acquired = true; }),
                              universal_receiver{});
    async::start(op2);
    CHECK_FALSE(second_acquired);

    // Release unblocks the second
    CHECK((async::just() | region::release<name>() | async::sync_wait())
              .has_value());
    CHECK(second_acquired);
    CHECK(region::detail::waiters_<name> > 0);

    // Clean up
    CHECK((async::just() | region::release<name>() | async::sync_wait())
              .has_value());
    CHECK(region::detail::waiters_<name> == 0);
}
