// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include <hal/interrupt_flows.hpp>

namespace interrupt {
struct module {
    constexpr static auto config = cib::config( //
        cib::exports<                           //
            isr_dma1_ch2                        //
            >);
};
} // namespace interrupt
