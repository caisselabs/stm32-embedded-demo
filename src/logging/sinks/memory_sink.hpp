// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// A log sink that keeps encoded packets in a RAM ring buffer.
//
// This is the sink that needs no hardware: it works on a bare board with
// nothing but a debugger attached, and it keeps the most recent packets across
// a halt. Dump it with
//
//     (gdb) p app_log::buffer.total_words
//     (gdb) dump binary value log.bin app_log::buffer
//
// then decode with tools/unwrap_log.py -- see docs/logging.org.
//
#pragma once

#include <stdx/span.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace app_log {

// Number of 32-bit words retained by the RAM log buffer.
constexpr inline std::size_t buffer_capacity = 256;

struct buffer_t {
   // 'C' 'I' 'B' 'L' -- a marker to find the buffer in a raw memory dump.
   std::uint32_t magic{0x4342494cU};
   std::uint32_t capacity{buffer_capacity};
   // Monotonically increasing count of words written. The oldest retained
   // word is at index `total_words - capacity` (modulo `capacity`) once
   // `total_words` exceeds `capacity`.
   std::uint32_t total_words{};
   std::array<std::uint32_t, buffer_capacity> words{};
};

inline buffer_t buffer{};

struct memory_sink {
   // Nothing to bring up -- the buffer is statically initialized. Present so
   // that every sink offers the same interface and app_log::init_sinks() can
   // call it uniformly.
   static auto init() -> void {}

   // The call operator is a template, so it is instantiated once per packet
   // size. `logging::binary::log_writer` wraps the call in a critical section
   // (see hal::concurrency_policy), so logging from an ISR is safe.
   template <std::size_t N>
   auto operator()(stdx::span<std::uint32_t const, N> packet) const -> void {
      for (auto const word : packet) {
         buffer.words[buffer.total_words % buffer_capacity] = word;
         ++buffer.total_words;
      }
   }
};

} // namespace app_log
