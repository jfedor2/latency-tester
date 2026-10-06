// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

// Custom boards define these in their board header; pico/pico2 use the
// defaults. D- is always D+ + 1. The sniffer samples 3 consecutive GPIOs, so
// the button must sit right before or right after the D+/D- pair.
#ifndef TESTER_DP_PIN
#ifdef PICO_DEFAULT_PIO_USB_DP_PIN
#define TESTER_DP_PIN PICO_DEFAULT_PIO_USB_DP_PIN
#else
#define TESTER_DP_PIN 0
#endif
#endif
#ifndef TESTER_BUTTON_PIN
#define TESTER_BUTTON_PIN 2
#endif

inline constexpr unsigned kDpPin = TESTER_DP_PIN;
inline constexpr unsigned kButtonPin = TESTER_BUTTON_PIN;
inline constexpr bool kButtonBeforeDp = (kButtonPin == kDpPin - 1);

static_assert(kButtonPin == kDpPin - 1 || kButtonPin == kDpPin + 2,
    "TESTER_BUTTON_PIN must be either TESTER_DP_PIN - 1 or TESTER_DP_PIN + 2");
