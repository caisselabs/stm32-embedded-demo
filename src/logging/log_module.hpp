// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// The cib component that brings the log sinks up.
//
#pragma once

#include <cib/cib.hpp>
#include <flow/step.hpp>

#include <stdx/tuple_algorithms.hpp>

#include <base_flows.hpp>
#include <logging/log_config.hpp>

namespace app_log {

struct module {
   // Bring up whatever hardware each enabled sink needs. A sink with nothing
   // to do (the RAM ring) supplies an empty init().
   constexpr static auto INIT_SINKS = flow::action<"app_log::INIT_SINKS">(
       [] { stdx::for_each([]<typename S>(S) { S::init(); }, enabled_sinks); });

   constexpr static auto config = cib::config( //
       cib::extend<base_flow::pre_init>(*INIT_SINKS));
};

} // namespace app_log
