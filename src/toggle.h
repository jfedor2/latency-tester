// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#ifndef _TOGGLE_H_
#define _TOGGLE_H_

#include <cstdint>

// Hardware-timed button toggling: a PIO1 state machine watches D+ for the next
// SOF and flips the button pin a given number of cycles after it.

// Claims a PIO1 state machine and takes ownership of the button pin.
void toggle_init();

// Flips the button pin `wait_cycles` clk_sys cycles (plus a small fixed delay)
// after the next SOF.
void toggle_arm(uint32_t wait_cycles);

// Blocks until `delay_cycles` after the next SOF without touching the pin.
// Returns false (and resets the state machine) if no SOF comes within
// `timeout_us`. Only call while nothing is armed.
bool toggle_sync_to_sof(uint32_t delay_cycles, uint32_t timeout_us);

// Flips the button pin immediately. Only call once the armed toggle is known
// to have fired, otherwise it presses the button instead of releasing it.
void toggle_reset();

// Returns to the initial state (button released, nothing armed) regardless
// of whether an armed toggle has fired.
void toggle_abort();

#endif
