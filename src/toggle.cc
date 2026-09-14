// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "toggle.h"

#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/time.h"

#include "tester_pins.h"
#include "toggle.pio.h"

namespace {

// How long D+ must stay idle to count as the end-of-frame gap rather than an
// inter-packet gap.
constexpr uint32_t kDebounceUs = 20;

// Raised by toggle.pio after every toggle.
constexpr uint kSyncIrq = 0;

PIO toggle_pio = pio1;
uint toggle_sm;
uint toggle_offset;
pio_sm_config toggle_config;
uint32_t toggle_debounce_iters;

// Resets to button released, nothing armed. Safe to call at any time.
void toggle_start() {
    pio_sm_set_enabled(toggle_pio, toggle_sm, false);
    pio_interrupt_clear(toggle_pio, kSyncIrq);

    // Released is high-Z (external pull-up); pressed is driving the output
    // value, which is fixed at 0 below.
    pio_sm_set_consecutive_pindirs(toggle_pio, toggle_sm, kButtonPin, 1, false);
    pio_sm_init(toggle_pio, toggle_sm, toggle_offset, &toggle_config);
    pio_sm_exec(toggle_pio, toggle_sm, pio_encode_set(pio_pins, 0));

    pio_sm_set_enabled(toggle_pio, toggle_sm, true);
    pio_sm_put_blocking(toggle_pio, toggle_sm, toggle_debounce_iters);
}

}  // namespace

void toggle_init() {
    toggle_sm = pio_claim_unused_sm(toggle_pio, true);
    toggle_offset = pio_add_program(toggle_pio, &toggle_at_sof_program);

    toggle_config = toggle_at_sof_program_get_default_config(toggle_offset);
    sm_config_set_jmp_pin(&toggle_config, kDpPin);
    sm_config_set_in_pins(&toggle_config, kDpPin);
    sm_config_set_out_pins(&toggle_config, kButtonPin, 1);
    sm_config_set_set_pins(&toggle_config, kButtonPin, 1);
    sm_config_set_clkdiv(&toggle_config, 1.0f);

    pio_gpio_init(toggle_pio, kButtonPin);

    uint32_t const clk_hz = clock_get_hz(clk_sys);
    uint32_t const debounce_cycles = (uint32_t) (((uint64_t) clk_hz * kDebounceUs) / 1000000ULL);
    toggle_debounce_iters = debounce_cycles / 2;  // the debounce loop is 2 instructions

    toggle_start();
}

void toggle_abort() {
    toggle_start();
}

void toggle_arm(uint32_t wait_cycles) {
    pio_sm_put_blocking(toggle_pio, toggle_sm, wait_cycles);
}

bool toggle_sync_to_sof(uint32_t delay_cycles, uint32_t timeout_us) {
    // Run the normal toggle path with OUT pointed at no pins, then undo the
    // direction bit flip in ISR.
    pio_interrupt_clear(toggle_pio, kSyncIrq);
    pio_sm_set_out_pins(toggle_pio, toggle_sm, kButtonPin, 0);
    pio_sm_put_blocking(toggle_pio, toggle_sm, delay_cycles);

    uint64_t const deadline_us = time_us_64() + timeout_us;
    while (!pio_interrupt_get(toggle_pio, kSyncIrq)) {
        if (time_us_64() > deadline_us) {
            toggle_start();  // also restores the OUT pins
            return false;
        }
    }
    pio_interrupt_clear(toggle_pio, kSyncIrq);
    pio_sm_set_out_pins(toggle_pio, toggle_sm, kButtonPin, 1);
    pio_sm_exec_wait_blocking(toggle_pio, toggle_sm, pio_encode_mov_not(pio_isr, pio_isr));
    return true;
}

void toggle_reset() {
    // The same instructions toggle.pio uses, so the two always agree on the
    // pin's current direction.
    pio_sm_exec_wait_blocking(toggle_pio, toggle_sm, pio_encode_mov_not(pio_isr, pio_isr));
    pio_sm_exec_wait_blocking(toggle_pio, toggle_sm, pio_encode_mov(pio_osr, pio_isr));
    pio_sm_exec_wait_blocking(toggle_pio, toggle_sm, pio_encode_out(pio_pindirs, 1));
}
