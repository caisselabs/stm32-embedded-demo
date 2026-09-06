// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once
#include <cib/cib.hpp>
#include <interrupt/module.hpp>

#include <base_flows.hpp>
#include <logging/log_module.hpp>

struct project_nexus {
   constexpr static auto config = //
       cib::components<           //
           base_flow::module,     //
           app_log::module,       //
           interrupt::module      //
           >;
};
