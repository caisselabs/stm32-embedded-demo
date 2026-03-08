// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include <groov/groov.hpp>
#include <groov/mmio_bus.hpp>

namespace interrupt {

using register_group = groov::group<"interrupt_group", groov::mmio_bus<>>;

}
