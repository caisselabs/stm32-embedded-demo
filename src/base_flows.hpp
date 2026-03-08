// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include <flow/service.hpp>

namespace base_flow {

struct pre_init : public flow::service<"base_flow::pre_init"> {};

struct init : public flow::service<"base_flow::init"> {};

struct start : public flow::service<"base_flow::start"> {};

struct loop : public flow::service<"base_flow::loop"> {};

struct sleep : public flow::service<"base_flow::sleep"> {};

} // namespace base_flow
