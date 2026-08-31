// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
#include <cib/cib.hpp>
#include <flow/run.hpp>
#include <interrupt/manager.hpp>
#include <interrupt/registers.hpp>

#include <app.hpp>
#include <base_flows.hpp>
#include <hal/concurrency_hal.hpp>
#include <hal/interrupt_hal.hpp>
#include <interrupt_config.hpp>
#include <logging/log_config.hpp>
#include <project.hpp>

extern "C" {
// called by startup code prior to main
void SystemInit() {}
}

using nexus_t = cib::nexus<project_nexus>;
constexpr nexus_t nexus{};
constexpr auto interrupt_manager =
    interrupt::manager<interrupt::config, hal::interrupt_hal<interrupt::register_group>,
                       nexus_t>{};

[[noreturn]] auto app_run() -> void {
   // Scopes every log below to the "app" module ID rather than the default.
   CIB_LOG_MODULE("app");

   CIB_INFO("Booting");

   nexus.init();
   flow::run<base_flow::pre_init>();

   interrupt_manager.init();

   flow::run<base_flow::init>();

   flow::run<base_flow::start>();

   hal::enable_interrupts();

   CIB_INFO("Entering main loop");

   while (true) {
      flow::run<base_flow::loop>();
   }
}
