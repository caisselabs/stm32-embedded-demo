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
#include <stdx/tuple.hpp>
#include <stdx/type_traits.hpp>

#include <cstdint>

#include <logging/sinks/memory_sink.hpp>
#include <logging/sinks/usart2_sink.hpp>

namespace app_log {

// The list of sinks for logging
constexpr auto enabled_sinks = stdx::tuple{memory_sink{}, usart2_sink{}};

// Sinks are brought up by app_log::module (logging/log_module.hpp), which
// registers a step in the base_flow::pre_init flow. That flow is declared with
// flow::log_policies::none, so nothing logs before the sinks exist.

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
inline auto logging::config<> = app_log::enabled_sinks.apply(
    [](auto... sinks) { return logging::binary::config{sinks...}; });

template <>
inline auto version::config<> = app_log::version_config{};
