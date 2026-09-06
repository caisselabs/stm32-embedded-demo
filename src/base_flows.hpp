// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include <cib/cib.hpp>
#include <flow/service.hpp>

namespace base_flow {

struct pre_init : public flow::service<"base_flow::pre_init", flow::log_policies::none> {};

struct init : public flow::service<"base_flow::init"> {};

struct start : public flow::service<"base_flow::start"> {};

struct loop : public flow::service<"base_flow::loop"> {};

struct sleep : public flow::service<"base_flow::sleep"> {};

struct module {
   constexpr static auto config = cib::config( //
       cib::exports<                           //
           pre_init,                           //
           init,                               //
           start,                              //
           loop,                               //
           sleep                               //
           >);
};

} // namespace base_flow
