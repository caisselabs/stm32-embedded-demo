// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// Deliberately a stub: it must contain no log call sites. The binary logger
// catalogues strings by extracting undefined symbols from `demo_lib`, and this
// translation unit is compiled into the executable rather than that library.
//
#include <app.hpp>

auto main() -> int { app_run(); }
