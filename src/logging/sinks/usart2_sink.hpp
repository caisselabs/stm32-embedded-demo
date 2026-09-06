// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// A log sink that writes encoded packets out USART2.
//
// USART2 is the NUCLEO-L432KC's ST-LINK virtual COM port, so this puts the log
// stream on /dev/ttyACM0 in Linux where tools/logmon can decode it live:
//
//     python3 tools/logmon --port --catalog build/log_strings.json
//
// The wire format is deliberately nothing: each packet word goes out as four
// little-endian bytes, back to back, with no framing, length or sync pattern.
// That keeps the firmware side to a loop and a register poll. logmon recovers
// word alignment by scoring the four possible byte offsets and resynchronizes
// at word granularity when it hits something it cannot decode -- see
// docs/logging.org.
//
#pragma once

#include <stdx/span.hpp>

#include <cstddef>
#include <cstdint>

#include <hal/usart_hal.hpp>

namespace app_log {

struct usart2_sink {
   constexpr static std::uint32_t baud = 115200;

   static auto init() -> void { hal::usart2_tx::init(baud); }

   // Called from inside a critical section (see hal::concurrency_policy), and
   // hal::usart2_tx::write() blocks until the transmit register is free. A log
   // call site therefore holds interrupts off for roughly 350us per packet
   // word at 115200 baud. See the note in usart_hal.hpp.
   template <std::size_t N>
   auto operator()(stdx::span<std::uint32_t const, N> packet) const -> void {
      for (auto const word : packet) {
         hal::usart2_tx::write(static_cast<std::uint8_t>(word));
         hal::usart2_tx::write(static_cast<std::uint8_t>(word >> 8U));
         hal::usart2_tx::write(static_cast<std::uint8_t>(word >> 16U));
         hal::usart2_tx::write(static_cast<std::uint8_t>(word >> 24U));
      }
   }
};

} // namespace app_log
