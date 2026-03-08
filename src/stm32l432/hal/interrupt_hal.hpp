// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
// ----------------------------------------------------------------------------
// Cortex-M4 NVIC interrupt HAL for CIB's interrupt manager and
// dynamic controller.
//
// Satisfies the interrupt::archetypes::hal concept from
// compile-time-init-build (CIB).
// ----------------------------------------------------------------------------
#pragma once

#include <interrupt/fwd.hpp>
#include <interrupt/policies.hpp>

#include <groov/groov.hpp>

#include <stdx/atomic.hpp>
#include <stdx/compiler.hpp>
#include <stdx/concepts.hpp>
#include <stdx/ct_string.hpp>

#include <cstddef>
#include <cstdint>

namespace hal {
namespace detail {

// -----------------------------------------------------------------------
// Cortex-M4 NVIC register addresses
// -----------------------------------------------------------------------
// Interrupt Set-Enable Registers   (ISER0..ISER7)
constexpr inline std::uint32_t NVIC_ISER_BASE = 0xe000'e100;
// Interrupt Clear-Enable Registers (ICER0..ICER7)
constexpr inline std::uint32_t NVIC_ICER_BASE = 0xe000'e180;
// Interrupt Priority Registers     (IPR0..IPR59, 8 bits per IRQ)
constexpr inline std::uint32_t NVIC_IPR_BASE = 0xe000'e400;
// Software Trigger Interrupt Register
// constexpr inline std::uint32_t NVIC_STIR = 0xe000'ef00;

// initial condition is that we are disabled
inline constinit std::atomic<std::uint32_t> disable_count = {1};

// Each ISER/ICER register covers 32 IRQs.
// For IRQ N: register index = N / 32, bit position = N % 32
template <interrupt::irq_num_t irq_num> inline void nvic_enable_irq() {
    constexpr auto irq_number = static_cast<std::uint32_t>(irq_num);
    constexpr auto reg_index = irq_number / 32u;
    constexpr auto bit_pos = irq_number % 32u;
    auto reg = reinterpret_cast<std::uint32_t volatile *>(NVIC_ISER_BASE +
                                                          reg_index * 4u);
    *reg = (1u << bit_pos);
}

template <interrupt::irq_num_t irq_num> inline void nvic_disable_irq() {
    constexpr auto irq_number = static_cast<std::uint32_t>(irq_num);
    constexpr auto reg_index = irq_number / 32u;
    constexpr auto bit_pos = irq_number % 32u;
    auto reg = reinterpret_cast<std::uint32_t volatile *>(NVIC_ICER_BASE +
                                                          reg_index * 4u);
    *reg = (1u << bit_pos);
}

// Cortex-M4 uses the upper bits of the 8-bit priority field.
// STM32L4 implements 4 priority bits (upper nibble). The priority
// value from the interrupt config is shifted into the implemented
// bits.
template <interrupt::irq_num_t irq_number, std::size_t priority>
inline void nvic_set_priority() {
    // IPR registers are byte-accessible; each IRQ has one byte
    auto reg = reinterpret_cast<std::uint8_t volatile *>(
        NVIC_IPR_BASE + static_cast<std::uint32_t>(irq_number));
    // STM32L4 implements 4 bits of priority in the upper nibble
    *reg = static_cast<std::uint8_t>(priority << 4u);
}

} // namespace detail

inline void enable_interrupts() {
    if (detail::disable_count.fetch_sub(1) == 1) {
        __asm__ __volatile__("cpsie i");
    }
    __asm__ __volatile__("" ::: "memory"); // compiler barrier
}

inline void disable_interrupts() {
    __asm__ __volatile__("" ::: "memory"); // compiler barrier
    if (detail::disable_count.fetch_add(1) == 0) {
        __asm__ __volatile__("cpsid i");
    }
}

struct disable_interrupts_lock {
    disable_interrupts_lock() { disable_interrupts(); }
    ~disable_interrupts_lock() { enable_interrupts(); }
};

inline void trigger_interrupt(int v) {
    // interrupt set-enable register
    auto NVIC_STIR = (std::uint32_t volatile *const)(0xe000'ef00);
    *NVIC_STIR = v;
}

// -----------------------------------------------------------------------
// CIB interrupt HAL for Cortex-M4 / STM32
//
// Template parameter PeripheralGroup is a groov register group used
// by the dynamic controller for peripheral-level interrupt enable and
// status fields (e.g. I2C interrupt enables, timer interrupt enables).
// -----------------------------------------------------------------------
template <typename PeripheralGroup> struct interrupt_hal {

    static auto init() -> void {}

    template <bool Enable, interrupt::irq_num_t IrqNumber, std::size_t Priority>
    static auto irq_init() -> void {
        detail::nvic_set_priority<IrqNumber, Priority>();

        if constexpr (Enable) {
            detail::nvic_enable_irq<IrqNumber>();
        } else {
            detail::nvic_disable_irq<IrqNumber>();
        }
    }

    template <interrupt::status_policy P>
    static auto run(interrupt::irq_num_t,
                    stdx::invocable auto const &isr) -> void {
        P::run([] {}, // function to clear the status, if not level sensitive
               [&] { isr(); } // function to run the isr
        );
    }

    // -------------------------------------------------------------------
    // Dynamic controller support
    //
    // These functions allow the CIB dynamic controller to
    // enable/disable peripheral-level interrupt sources at runtime
    // by manipulating enable fields in peripheral registers via groov.
    // -------------------------------------------------------------------

    // Given a field type (e.g. an enable_field_t from the interrupt
    // config), resolve it to a groov path.
    template <typename Field>
    CONSTEVAL static auto get_field() -> groov::pathlike auto {
        return groov::make_path<Field::value>();
    }

    // Return the parent register path for a given field.
    template <typename Field>
    CONSTEVAL static auto get_register() -> groov::pathlike auto {
        return groov::parent(get_field<Field>());
    }
    // The underlying data type of the register (e.g. std::uint32_t).
    template <groov::pathlike Register>
    using register_datatype_t = typename decltype(groov::resolve(
        PeripheralGroup{}, Register{}))::type_t;

    // Bitmask for a specific field within its register.
    template <groov::pathlike Register, typename Field>
    constexpr static register_datatype_t<Register> mask =
        groov::resolve(PeripheralGroup{}, groov::make_path<Field::value>())
            .template mask<register_datatype_t<Register>>;

    // Write a raw value to a peripheral register via groov.
    template <groov::pathlike Register>
    static auto write(auto raw_value) -> void {
        groov::sync_write(PeripheralGroup{}(Register{} = raw_value));
    }
};

} // namespace hal
