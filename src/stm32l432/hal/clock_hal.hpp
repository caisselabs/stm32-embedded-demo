// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// System clock bring-up: MSI 4 MHz (reset default) -> PLL 80 MHz.
//
//   HSI16 --> /PLLM=2 --> 8 MHz --> xPLLN=20 --> 160 MHz --> /PLLR=2 --> 80 MHz
//
// The VCO input has to land between 4 and 16 MHz and the VCO output between
// 64 and 344 MHz (RM0394 6.4.4); 8 MHz and 160 MHz sit comfortably inside
// both. HSI16 rather than MSI as the source because HSI16 is already running
// as the USART and I2C kernel clock, so the PLL costs no extra oscillator.
//
// Two peripherals would otherwise have made this painful and neither does,
// because both were deliberately put on HSI16 as a *kernel* clock rather than
// on PCLK:
//
//   - USART2 (hal/usart_hal.hpp) -- BRR is a compile-time constant and the
//     log transport is unaffected by anything here.
//   - I2C1 (ccipr.I2C1SEL) -- the hand-tuned TIMINGR word stays valid.
//
// What *is* affected is anything counting PCLK: see src/shared/stm32_timer.hpp,
// where TIM2 is prescaled back down to the tick rate the timer scheduler
// declares.
//
#pragma once

#include <groov/groov.hpp>

#include <cstdint>

#include <caisselabs/stm32/stm32l432.hpp>

namespace hal {

struct clock {
   // The whole tree runs undivided: AHB, APB1 and APB2 are all rated to
   // 80 MHz on this part, so there is nothing to gain by prescaling.
   constexpr static std::uint32_t sysclk_hz = 80'000'000U;
   constexpr static std::uint32_t hclk_hz = sysclk_hz;
   constexpr static std::uint32_t pclk1_hz = sysclk_hz;
   constexpr static std::uint32_t pclk2_hz = sysclk_hz;

   // The APB timer clock is PCLK when the APB prescaler is 1 and twice PCLK
   // otherwise. Timer code should size its prescaler against these rather
   // than against pclk*_hz, so that dividing an APB bus later shows up as an
   // arithmetic change in one place instead of a silent doubling.
   constexpr static std::uint32_t apb1_timer_hz = pclk1_hz;
   constexpr static std::uint32_t apb2_timer_hz = pclk2_hz;

   // 16 MHz / 2 * 20 / 2. PLLN is the only one that is a plain number.
   constexpr static std::uint8_t pll_n = 20U;

   // Called from SystemInit, before main and before anything else touches a
   // peripheral. Returns immediately if the PLL is already the system clock,
   // which makes a second call harmless -- and matters, because the sequence
   // below disables the PLL to reprogram it and doing that while running from
   // it would stop the core.
   static auto init() -> void {
      using namespace groov::literals;
      namespace stm32 = caisselabs::stm32;

      if (sysclk_is_pll()) {
         return;
      }

      // Flash latency leads the frequency change, and RM0394 3.3.3 requires
      // reading it back before the switch rather than assuming the write
      // landed. Four wait states covers HCLK up to 80 MHz in VCORE Range 1
      // (Table 9), which is the range the part resets into -- PWR needs no
      // attention at all here.
      //
      // Prefetch is off at reset and is what makes multiple wait states
      // tolerable; the instruction and data caches are already on.
      groov::sync_write(stm32::flash("acr.LATENCY"_f = stm32::flashx::latency::ws4,
                                     "acr.PRFTEN"_f = groov::enable));
      while (not latency_is(stm32::flashx::latency::ws4)) {
      }

      // HSI16 has to be running and stable before the PLL can use it. It is
      // very likely already on -- the USART sink turns it on for its kernel
      // clock -- but SystemInit runs first, so do not assume it.
      groov::sync_write(stm32::rcc("cr.HSION"_f = groov::enable));
      while (not is_ready(groov::sync_read(stm32::rcc / "cr.HSIRDY"_f))) {
      }

      // PLLM, PLLN, PLLR and PLLSRC are writable only while the PLL is
      // disabled.
      groov::sync_write(stm32::rcc("cr.PLLON"_f = groov::disable));
      while (is_locked(groov::sync_read(stm32::rcc / "cr.PLLRDY"_f))) {
      }

      groov::sync_write(stm32::rcc("pllcfgr.PLLSRC"_f = stm32::rccx::pllsrc::HSI16,
                                   "pllcfgr.PLLM"_f = stm32::rccx::pllm::div2,
                                   "pllcfgr.PLLN"_f = pll_n,
                                   "pllcfgr.PLLR"_f = stm32::rccx::plldiv::div2,
                                   "pllcfgr.PLLREN"_f = groov::enable));

      groov::sync_write(stm32::rcc("cr.PLLON"_f = groov::enable));
      while (not is_locked(groov::sync_read(stm32::rcc / "cr.PLLRDY"_f))) {
      }

      // Spelled out rather than left at their reset values: the timer
      // prescaler in stm32_timer.hpp is only correct while PPRE1 is div1, and
      // a comment is easier to miss than a register write.
      groov::sync_write(stm32::rcc("cfgr.HPRE"_f = stm32::rccx::hpre::div1,  //
                                   "cfgr.PPRE1"_f = stm32::rccx::ppre::div1, //
                                   "cfgr.PPRE2"_f = stm32::rccx::ppre::div1));

      groov::sync_write(stm32::rcc("cfgr.SW"_f = stm32::rccx::sw::PLL));
      while (not sysclk_is_pll()) {
      }

      // MSI is left running. Nothing uses it now, but it is the default
      // wake-from-stop clock (cfgr.STOPWUCK) and switching that off is a
      // low-power-mode decision rather than a clock-speed one.
   }

 private:
   // No is_*() helper exists for these two field types, so the value is
   // pulled into a typed local before comparing rather than leaning on
   // sync_read's implicit conversion in an operator== context.
   static auto latency_is(caisselabs::stm32::flashx::latency want) -> bool {
      using namespace groov::literals;
      caisselabs::stm32::flashx::latency const v =
          groov::sync_read(caisselabs::stm32::flash / "acr.LATENCY"_f);
      return v == want;
   }

   static auto sysclk_is_pll() -> bool {
      using namespace groov::literals;
      caisselabs::stm32::rccx::sw const v =
          groov::sync_read(caisselabs::stm32::rcc / "cfgr.SWS"_f);
      return v == caisselabs::stm32::rccx::sw::PLL;
   }
};

} // namespace hal
