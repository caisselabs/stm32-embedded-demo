//
// Copyright (c) 2025 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// This file contains methods for handling tim2 in the STM32 which will be
// used for our timer_scheduler.
//
#pragma once

#include <caisselabs/stm32/stm32l432.hpp>

#include <groov/groov.hpp>

#include <cstdint>

#include <hal/clock_hal.hpp>

namespace stm32 = caisselabs::stm32;

using namespace groov::literals;

// The scheduler's time_point counts ticks of this rate -- see the duration in
// timer_scheduler.hpp, which is expressed against the same constant. TIM2 is
// prescaled to produce it exactly, so SYSCLK can move without the time type,
// the timeouts, or any test having to move with it.
inline constexpr std::uint32_t timer_tick_hz = 16'000'000U;

// CK_CNT = CK_PSC / (PSC + 1). CK_PSC here is the APB1 timer clock, which is
// PCLK1 only while PPRE1 is div1 -- hal::clock::init() writes that explicitly
// for exactly this reason.
static_assert(hal::clock::apb1_timer_hz % timer_tick_hz == 0,
              "TIM2 cannot divide the APB1 timer clock down to the scheduler "
              "tick exactly; pick a tick rate that divides it");
inline constexpr std::uint32_t tim2_psc =
    (hal::clock::apb1_timer_hz / timer_tick_hz) - 1U;

inline void initialize_timer() {

  auto init_tim2 =
    stm32::tim2 (
      "cr1.UIFREMAP"_f = false,
      "cr1.CKD"_f = 0x00,
      "cr1.ARPE"_f = true,
      "cr1.CMS"_f = 0x00,
      "cr1.DIR"_f = false,
      "cr1.OPM"_f = false,
      "cr1.URS"_f = false,
      "cr1.UDIS"_f = false,
      "cr1.CEN"_f = false,

      "psc.PSC"_f = tim2_psc,

      "dier.TIE"_f = true,
      "dier.CC1IE"_f = true,
      "dier.UIE"_f = false,

      "ccmr1_out.OC1M"_f = stm32::ocm_t::active_on_match,
      "ccmr1_out.OC1PE"_f = false,
      "ccmr1_out.OC1FE"_f = false,
      "ccmr1_out.OC1S"_f = stm32::ccsel_t::output,

      "ccer.CC1NP"_f = false,
      "ccer.CC1P"_f = false,
      "ccer.CC1E"_f = true
    );

  groov::sync_write(init_tim2);

  // PSC is buffered: the value written above only reaches the active
  // prescaler at an update event, and TIM2 free-runs all the way to
  // 0xffff'ffff before it raises one. Without forcing an update here the new
  // prescaler would not take effect for 268 seconds. UG reloads the shadow
  // registers and clears the counter; UIE is off, so nothing is interrupted.
  groov::sync_write(stm32::tim2("egr.UG"_f = true));
}

inline void enable_timer() {
    groov::sync_write(stm32::tim2("cr1.CEN"_f = true));
}  

inline void disable_timer() {
    groov::sync_write(stm32::tim2("cr1.CEN"_f = false));
}

inline void reset_timer() {
    groov::sync_write(stm32::tim2("cnt.CNT"_f = 0x00));
}

inline std::uint32_t get_timer_value() {
    auto count = groov::sync_read(stm32::tim2 / "cnt"_r);
    return count;
}

inline void start_timer(std::uint32_t target) {
    groov::sync_write(stm32::tim2("ccr1.CCR1"_f = target));

    // get things counting again
    enable_timer();
}

