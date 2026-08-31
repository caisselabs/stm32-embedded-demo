// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// Binary logging configuration for the demo.
//
// CIB's binary logger keeps no string data in the executable: each log call
// site sends a catalogued 32-bit string ID (plus any runtime arguments)
// encoded per the MIPI Sys-T spec. The IDs are assigned at build time by
// `gen_str_catalog` (see the top-level CMakeLists.txt), which also emits the
// JSON/XML collateral a decoder needs to reconstitute the text.
//
// IMPORTANT: every translation unit that logs must see this same
// specialization of `logging::config`, otherwise you have an ODR violation.
// Include this header (not <log/log.hpp>) from any .cpp that logs.
//
#pragma once

#include <log_binary/catalog/encoder.hpp>

#include <stdx/ct_string.hpp>
#include <stdx/span.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace app_log {

// Number of 32-bit words retained by the RAM log buffer.
constexpr inline std::size_t buffer_capacity = 256;

// The destination for encoded packets.
//
// This demo has no dedicated log transport yet, so packets land in a circular
// buffer in RAM that can be dumped from the debugger:
//
//     (gdb) p app_log::buffer.total_words
//     (gdb) dump binary value log.bin app_log::buffer.words
//
// Replace `log_destination::operator()` with a real transport (UART, SWO/ITM,
// ...) when one exists; nothing else in the logging setup has to change.
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

struct log_destination {
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

// Reported by CIB_LOG_VERSION(). `build_id` is what a decoder keys on to pick
// the matching string catalog, so bump it whenever the catalog changes.
//
// `version_string` is deliberately empty: a non-empty one makes the encoder
// emit a MIPI "long build" message, which puts the string itself in the image
// (defeating the point of binary logging) and hands the destination a byte
// span rather than a word span. Empty means a compact build-ID packet.
//
// Note that CIB_LOG_VERSION() emits a MIPI Build message (type 0), which the
// log_decode.py bundled with cib does not decode -- it handles Short32,
// Short64 and Catalog messages only. That is why the demo does not call it on
// the boot path.
struct version_config {
   constexpr static auto build_id = std::uint64_t{0x0001'0000};
   constexpr static auto version_string = stdx::ct_string{""};
};

} // namespace app_log

template <>
inline auto logging::config<> = logging::binary::config{app_log::log_destination{}};

template <>
inline auto version::config<> = app_log::version_config{};
