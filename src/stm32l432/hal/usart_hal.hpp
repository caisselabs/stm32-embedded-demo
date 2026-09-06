// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// Transmit-only USART2 driver.
//
// On the NUCLEO-L432KC, USART2 is wired to the ST-LINK virtual COM port, so
// bytes written here come out of the debug USB connection as /dev/ttyACM0 on
// the host. Only TX is configured: this exists to carry binary log packets off
// the board (see src/logging/sinks/usart2_sink.hpp), and nothing sends to the
// target.
//
//   PA2 -- USART2_TX (AF7)
//   PA15 - USART2_RX (AF3), deliberately left unconfigured
//
#pragma once

#include <groov/groov.hpp>

#include <cstdint>

#include <caisselabs/stm32/stm32l432.hpp>

namespace hal {

struct usart2_tx {
   // The USART kernel clock is switched to HSI16 rather than PCLK. HSI16 is a
   // fixed 16 MHz regardless of what the rest of the clock tree is doing, so
   // the baud rate divisor below is a constant and the log transport keeps
   // working if SYSCLK is later reconfigured. The cost is leaving HSI16
   // running.
   constexpr static std::uint32_t kernel_clock_hz = 16'000'000U;

   // Oversampling by 16 (OVER8 = 0), so USARTDIV is just fck/baud. Rounded to
   // nearest rather than truncated: at 115200 the exact quotient is 138.89, and
   // truncating to 138 is a 0.6% error in the wrong direction.
   constexpr static auto divisor(std::uint32_t baud) -> std::uint32_t {
      return (kernel_clock_hz + (baud / 2U)) / baud;
   }

   // Bring up the pin, the clocks and the peripheral. Safe to call more than
   // once; not safe to call concurrently with write().
   static auto init(std::uint32_t baud) -> void {
      using namespace groov::literals;
      namespace stm32 = caisselabs::stm32;

      // HSI16 has to be running and stable before it can be selected as the
      // USART kernel clock.
      groov::sync_write(stm32::rcc("cr.HSION"_f = groov::enable));
      while (not is_ready(groov::sync_read(stm32::rcc / "cr.HSIRDY"_f))) {
      }

      groov::sync_write(stm32::rcc("ahb2enr.GPIOAEN"_f = groov::enable,
                                   "apb1enr1.USART2EN"_f = groov::enable));

      // CCIPR is a read-modify-write register in groov (the fields it does not
      // mention are read back and preserved), so this does not disturb the
      // other peripheral clock selections.
      groov::sync_write(stm32::rcc("ccipr.UART2SEL"_f = stm32::rccx::usartclk::HSI16));

      // Park PA2 high as a plain GPIO output before the USART owns it.
      //
      // A USART drives its TX output low while the peripheral is disabled, so
      // switching the pin to alternate function first pulls the line low --
      // which is a start bit as far as the receiver is concerned. It stays low
      // until UE/TE are set a few register writes later. At 115200 that gap is
      // about five bit times, and the receiver frames it as one spurious 0xf0
      // byte: a start bit plus four low data bits, then high for the rest.
      // Every reset emitted one, and because it is a whole byte it also put
      // the entire rest of the stream out of word alignment.
      //
      // Driving the pin high first means the line is already idle when the
      // USART takes it over, so the handover is silent. ODR is set in the same
      // write as the pin configuration but MODER is left alone, so the output
      // driver only turns on once it has a 1 to drive.
      groov::sync_write(stm32::gpioa(                     //
          "odr.2"_f = true,                               //
          "otyper.2"_f = stm32::gpio::outtype::push_pull, //
          "ospeedr.2"_f = stm32::gpio::speed::high_speed, //
          "pupdr.2"_f = stm32::gpio::pupd::none,          //
          "afrl.2"_f = stm32::gpio::afsel::AF7));
      groov::sync_write(stm32::gpioa("moder.2"_f = stm32::gpio::mode::output));

      // BRR is only writable while the peripheral is disabled (RM0394 38.5.2),
      // so drop UE before programming it.
      groov::sync_write(stm32::usart2("cr1.UE"_f = false));
      groov::sync_write(stm32::usart2("brr.BRR"_f = divisor(baud)));

      // 8N1, no parity, oversampling by 16. These are all reset values, but
      // spelling them out means init() lands in a known state even when the
      // peripheral was left configured by something else (a previous run that
      // was not power cycled, say).
      groov::sync_write(stm32::usart2( //
          "cr1.M0"_f = false,          //
          "cr1.M1"_f = false,          //
          "cr1.OVER8"_f = false,       //
          "cr1.PCE"_f = false,         //
          "cr1.TE"_f = true,           //
          "cr1.UE"_f = true));

      // Enabling TE queues an idle frame; TEACK says the transmitter has
      // actually acknowledged the enable and TDR may be written.
      while (not groov::sync_read(stm32::usart2 / "isr.TEACK"_f)) {
      }

      // The USART is enabled and idling its TX high, so handing it the pin is
      // now a high-to-high transition. See the note above.
      groov::sync_write(stm32::gpioa("moder.2"_f = stm32::gpio::mode::alternate));
   }

   // Blocking single byte write. At 115200 baud this parks the CPU for roughly
   // 87us per byte, and the binary logger calls its destinations from inside a
   // critical section -- so a log call site costs about 350us per packet word
   // with interrupts disabled. That is acceptable for a demo transport and
   // deliberately simple; a buffered or DMA-fed version is the way out.
   static auto write(std::uint8_t byte) -> void {
      using namespace groov::literals;
      namespace stm32 = caisselabs::stm32;

      // block for the TX buffer to be empty
      while (not groov::sync_read(stm32::usart2 / "isr.TXE"_f)) {
      }
      groov::sync_write(stm32::usart2("tdr.TDR"_f = byte));
   }

   // Wait until the last byte has finished shifting out. write() returns as
   // soon as TDR is free, which is one character time before the wire is idle,
   // so anything that halts or resets the core immediately after logging (a
   // breakpoint, a fault handler) should drain() first or lose the tail.
   static auto drain() -> void {
      using namespace groov::literals;
      namespace stm32 = caisselabs::stm32;

      while (not groov::sync_read(stm32::usart2 / "isr.TC"_f)) {
      }
   }
};

} // namespace hal
