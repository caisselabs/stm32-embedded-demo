// Copyright (c) 2026 Michael Caisse
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once
#include <flow/service.hpp>

namespace interrupt {

// STM32L432 interrupt flow services
// IRQ numbers match the vector table in startup_gcc.s

struct isr_wwdg             : flow::service<"isr_wwdg"> {};             // IRQ 0
struct isr_pvd_pvm          : flow::service<"isr_pvd_pvm"> {};          // IRQ 1
struct isr_rtc_tamp_stamp   : flow::service<"isr_rtc_tamp_stamp"> {};   // IRQ 2
struct isr_rtc_wkup         : flow::service<"isr_rtc_wkup"> {};         // IRQ 3
struct isr_flash            : flow::service<"isr_flash"> {};            // IRQ 4
struct isr_rcc              : flow::service<"isr_rcc"> {};              // IRQ 5
struct isr_exti0            : flow::service<"isr_exti0"> {};            // IRQ 6
struct isr_exti1            : flow::service<"isr_exti1"> {};            // IRQ 7
struct isr_exti2            : flow::service<"isr_exti2"> {};            // IRQ 8
struct isr_exti3            : flow::service<"isr_exti3"> {};            // IRQ 9
struct isr_exti4            : flow::service<"isr_exti4"> {};            // IRQ 10
struct isr_dma1_ch1         : flow::service<"isr_dma1_ch1"> {};         // IRQ 11
struct isr_dma1_ch2         : flow::service<"isr_dma1_ch2"> {};         // IRQ 12
struct isr_dma1_ch3         : flow::service<"isr_dma1_ch3"> {};         // IRQ 13
struct isr_dma1_ch4         : flow::service<"isr_dma1_ch4"> {};         // IRQ 14
struct isr_dma1_ch5         : flow::service<"isr_dma1_ch5"> {};         // IRQ 15
struct isr_dma1_ch6         : flow::service<"isr_dma1_ch6"> {};         // IRQ 16
struct isr_dma1_ch7         : flow::service<"isr_dma1_ch7"> {};         // IRQ 17
struct isr_adc1_2           : flow::service<"isr_adc1_2"> {};           // IRQ 18
struct isr_can1_tx          : flow::service<"isr_can1_tx"> {};          // IRQ 19
struct isr_can1_rx0         : flow::service<"isr_can1_rx0"> {};         // IRQ 20
struct isr_can1_rx1         : flow::service<"isr_can1_rx1"> {};         // IRQ 21
struct isr_can1_sce         : flow::service<"isr_can1_sce"> {};         // IRQ 22
struct isr_exti9_5          : flow::service<"isr_exti9_5"> {};          // IRQ 23
struct isr_tim15            : flow::service<"isr_tim15"> {};            // IRQ 24
struct isr_tim16            : flow::service<"isr_tim16"> {};            // IRQ 25
struct isr_tim1_trg_com     : flow::service<"isr_tim1_trg_com"> {};     // IRQ 26
struct isr_tim1_cc          : flow::service<"isr_tim1_cc"> {};          // IRQ 27
struct isr_tim2             : flow::service<"isr_tim2"> {};             // IRQ 28
struct isr_tim3             : flow::service<"isr_tim3"> {};             // IRQ 29
// IRQ 30 reserved
struct isr_i2c1_ev          : flow::service<"isr_i2c1_ev"> {};          // IRQ 31
struct isr_i2c1_er          : flow::service<"isr_i2c1_er"> {};          // IRQ 32
struct isr_i2c2_ev          : flow::service<"isr_i2c2_ev"> {};          // IRQ 33
struct isr_i2c2_er          : flow::service<"isr_i2c2_er"> {};          // IRQ 34
struct isr_spi1             : flow::service<"isr_spi1"> {};             // IRQ 35
struct isr_spi2             : flow::service<"isr_spi2"> {};             // IRQ 36
struct isr_usart1           : flow::service<"isr_usart1"> {};           // IRQ 37
struct isr_usart2           : flow::service<"isr_usart2"> {};           // IRQ 38
struct isr_usart3           : flow::service<"isr_usart3"> {};           // IRQ 39
struct isr_exti15_10        : flow::service<"isr_exti15_10"> {};        // IRQ 40
struct isr_rtc_alarm        : flow::service<"isr_rtc_alarm"> {};        // IRQ 41
// IRQ 42-48 reserved
struct isr_sdmmc1           : flow::service<"isr_sdmmc1"> {};           // IRQ 49
// IRQ 50 reserved
struct isr_spi3             : flow::service<"isr_spi3"> {};             // IRQ 51
struct isr_uart4            : flow::service<"isr_uart4"> {};            // IRQ 52
// IRQ 53 reserved
struct isr_tim6_dacunder    : flow::service<"isr_tim6_dacunder"> {};    // IRQ 54
struct isr_tim7             : flow::service<"isr_tim7"> {};             // IRQ 55
struct isr_dma2_ch1         : flow::service<"isr_dma2_ch1"> {};         // IRQ 56
struct isr_dma2_ch2         : flow::service<"isr_dma2_ch2"> {};         // IRQ 57
struct isr_dma2_ch3         : flow::service<"isr_dma2_ch3"> {};         // IRQ 58
struct isr_dma2_ch4         : flow::service<"isr_dma2_ch4"> {};         // IRQ 59
struct isr_dma2_ch5         : flow::service<"isr_dma2_ch5"> {};         // IRQ 60
struct isr_dfsdm1_flt0      : flow::service<"isr_dfsdm1_flt0"> {};      // IRQ 61
struct isr_dfsdm1_flt1      : flow::service<"isr_dfsdm1_flt1"> {};      // IRQ 62
// IRQ 63 reserved
struct isr_comp             : flow::service<"isr_comp"> {};             // IRQ 64
struct isr_lptim1           : flow::service<"isr_lptim1"> {};           // IRQ 65
struct isr_lptim2           : flow::service<"isr_lptim2"> {};           // IRQ 66
struct isr_usb_fs           : flow::service<"isr_usb_fs"> {};           // IRQ 67
struct isr_dma2_ch6         : flow::service<"isr_dma2_ch6"> {};         // IRQ 68
struct isr_dma2_ch7         : flow::service<"isr_dma2_ch7"> {};         // IRQ 69
struct isr_lpuart1          : flow::service<"isr_lpuart1"> {};          // IRQ 70
struct isr_quadspi          : flow::service<"isr_quadspi"> {};          // IRQ 71
struct isr_i2c3_ev          : flow::service<"isr_i2c3_ev"> {};          // IRQ 72
struct isr_i2c3_er          : flow::service<"isr_i2c3_er"> {};          // IRQ 73
struct isr_sai1             : flow::service<"isr_sai1"> {};             // IRQ 74
// IRQ 75 reserved
struct isr_swpmi1           : flow::service<"isr_swpmi1"> {};           // IRQ 76
struct isr_tsc              : flow::service<"isr_tsc"> {};              // IRQ 77
struct isr_lcd              : flow::service<"isr_lcd"> {};              // IRQ 78
struct isr_aes              : flow::service<"isr_aes"> {};              // IRQ 79
struct isr_rng              : flow::service<"isr_rng"> {};              // IRQ 80
struct isr_fpu              : flow::service<"isr_fpu"> {};              // IRQ 81
struct isr_crs              : flow::service<"isr_crs"> {};              // IRQ 82
struct isr_i2c4_ev          : flow::service<"isr_i2c4_ev"> {};          // IRQ 83
struct isr_i2c4_er          : flow::service<"isr_i2c4_er"> {};          // IRQ 84

} // namespace interrupt
