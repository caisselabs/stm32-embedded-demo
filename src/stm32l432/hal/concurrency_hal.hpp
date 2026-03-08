// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once
#include <conc/concurrency.hpp>

#include <functional>

#include <hal/interrupt_hal.hpp>

namespace hal {
struct concurrency_policy {
    template <typename = void, std::invocable F, std::predicate... Pred>
    static auto call_in_critical_section(F &&f, Pred &&...pred)
        -> decltype(std::forward<F>(f)()) {
        while (true) {
            [[maybe_unused]] hal::disable_interrupts_lock lock{};
            if ((... and pred())) {
                return std::forward<F>(f)();
            }
        }
    }
};
} // namespace hal

template <> inline auto conc::injected_policy<> = hal::concurrency_policy{};
