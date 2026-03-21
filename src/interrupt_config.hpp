// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once
#include <flow/service.hpp>
#include <interrupt/config.hpp>

#include <hal/interrupt_flows.hpp>

namespace interrupt {
using interrupt::operator""_irq;

// clang-format off
using config =
    interrupt::root<
        irq<"irq_12", 12_irq, 4, policies<>, isr_dma1_ch2>
    >;
// clang-format on
} // namespace interrupt
