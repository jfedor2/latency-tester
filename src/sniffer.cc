// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "sniffer.h"

#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

#include "sniffer.pio.h"
#include "tester_pins.h"

// Decoding a capture:
//   1. Find the button edge.
//   2. Find packets on D+/D- (non-idle intervals ending in an SE0 EOP).
//   3. Decode each packet's PID.
//   4. Take the last DATA packet before the mark as the response, and the
//      IN and SOF before it.

namespace {

constexpr uint32_t kCaptureTotalUs = 5000;
constexpr uint32_t kAutopushThreshold = 30;
constexpr uint32_t kSamplesPerWord = 10;
constexpr uint32_t kWordPadBits = 32 - kAutopushThreshold;
constexpr uint32_t kClkDiv = 2;
constexpr uint32_t kMaxCaptureWords = 40000;

constexpr uint8_t kPidSof = 0xA5;
constexpr uint8_t kPidIn = 0x69;
constexpr uint8_t kPidData0 = 0xC3;
constexpr uint8_t kPidData1 = 0x4B;

constexpr uint32_t kSampleBase = kButtonBeforeDp ? (kDpPin - 1) : kDpPin;
constexpr uint32_t kBitButton = kButtonBeforeDp ? 0 : 2;
constexpr uint32_t kBitDp = kButtonBeforeDp ? 1 : 0;
constexpr uint32_t kBitDm = kButtonBeforeDp ? 2 : 1;

PIO sniffer_pio = pio1;
uint sniffer_sm;
int dma_chan;
uint32_t sample_hz;
// Samples per USB full-speed bit, fixed point (6.5 at 78 MHz).
constexpr uint32_t kBitFracBits = 8;
uint32_t samples_per_bit_fx;
uint32_t capture_words;
uint32_t mark_words;

uint32_t capture_buffer[kMaxCaptureWords];

// With a right-shifting ISR the samples end up in bits [31:kWordPadBits],
// oldest lowest.
inline uint32_t get_sample(const uint32_t* buf, uint32_t i) {
    uint32_t word = buf[i / kSamplesPerWord];
    uint32_t shift = kWordPadBits + 3 * (i % kSamplesPerWord);
    return (word >> shift) & 0x7u;
}

inline bool get_button(uint32_t sample) {
    return (sample >> kBitButton) & 0x1u;
}

// Lets the packet search skip over a word of idle samples at once.
constexpr uint32_t make_idle_word_mask() {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < kSamplesPerWord; i++) {
        mask |= (((1u << kBitDp) | (1u << kBitDm)) << (kWordPadBits + 3 * i));
    }
    return mask;
}
constexpr uint32_t make_idle_word_value() {
    // J reads as D+ 0, D- 1 (inverted).
    uint32_t value = 0;
    for (uint32_t i = 0; i < kSamplesPerWord; i++) {
        value |= ((1u << kBitDm) << (kWordPadBits + 3 * i));
    }
    return value;
}
constexpr uint32_t kIdleWordMask = make_idle_word_mask();
constexpr uint32_t kIdleWordValue = make_idle_word_value();

// noinline stops GCC from recomputing word_idx as i / kSamplesPerWord, which
// is a slow library call on Cortex-M0+.
__attribute__((noinline)) bool word_is_all_idle(const uint32_t* buf, uint32_t word_idx) {
    return (buf[word_idx] & kIdleWordMask) == kIdleWordValue;
}

// Lets the button edge search skip over a word with no button change at once.
constexpr uint32_t make_button_bit_mask() {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < kSamplesPerWord; i++) {
        mask |= ((1u << kBitButton) << (kWordPadBits + 3 * i));
    }
    return mask;
}
constexpr uint32_t kButtonBitMask = make_button_bit_mask();

// noinline for the same reason as word_is_all_idle().
__attribute__((noinline)) bool word_all_button_state(const uint32_t* buf, uint32_t word_idx, bool state) {
    uint32_t const masked = buf[word_idx] & kButtonBitMask;
    return state ? (masked == kButtonBitMask) : (masked == 0);
}

struct LineState {
    bool dp;
    bool dm;
};

inline LineState decode_line(uint32_t sample) {
    // The pins' input is inverted (GPIO_OVERRIDE_INVERT).
    bool raw_dp = (sample >> kBitDp) & 0x1u;
    bool raw_dm = (sample >> kBitDm) & 0x1u;
    return { !raw_dp, !raw_dm };
}

enum class LineSymbol { J,
    K,
    SE0,
    SE1 };

inline LineSymbol classify(LineState s) {
    if (s.dp && !s.dm) {
        return LineSymbol::J;
    }
    if (!s.dp && s.dm) {
        return LineSymbol::K;
    }
    if (!s.dp && !s.dm) {
        return LineSymbol::SE0;
    }
    return LineSymbol::SE1;
}

struct PacketInterval {
    uint32_t start;  // sample index of the first non-idle sample
    uint32_t end;    // sample index of the first SE0 sample of the EOP
    uint8_t pid;
};

// Decodes the PID of the packet in samples [start, end), or returns 0.
uint8_t decode_pid(const uint32_t* buf, uint32_t start, uint32_t end, uint32_t samples_per_bit_fx) {
    uint32_t const half_bit = samples_per_bit_fx / 2;

    LineSymbol prev_symbol = classify(decode_line(get_sample(buf, start)));
    uint32_t next_center = (start << kBitFracBits) + half_bit;

    uint32_t bits_extracted = 0;
    uint32_t consecutive_ones = 0;
    uint32_t data_bits = 0;
    uint32_t data_bit_count = 0;

    while (bits_extracted < 24 && data_bit_count < 8) {
        uint32_t center = (next_center + (1u << (kBitFracBits - 1))) >> kBitFracBits;  // rounded
        if (center >= end) {
            break;
        }

        LineSymbol sym = classify(decode_line(get_sample(buf, center)));
        bool const bit_is_one = (sym == prev_symbol);
        prev_symbol = sym;

        // Resync to the next transition, if there is one within a bit.
        uint32_t search_begin = (center > 0) ? center - 1 : 0;
        uint32_t search_end = center + (samples_per_bit_fx >> kBitFracBits) + 1;
        if (search_end > end) {
            search_end = end;
        }
        bool have_edge = false;
        uint32_t edge_at = 0;
        for (uint32_t s = search_begin + 1; s < search_end; s++) {
            if (classify(decode_line(get_sample(buf, s - 1))) !=
                classify(decode_line(get_sample(buf, s)))) {
                edge_at = s;
                have_edge = true;
                break;
            }
        }
        next_center = have_edge ? ((edge_at << kBitFracBits) + half_bit) : (next_center + samples_per_bit_fx);

        bits_extracted++;

        if (bits_extracted <= 8) {
            continue;  // SYNC
        }

        if (consecutive_ones >= 6) {
            // Stuffed bit.
            consecutive_ones = 0;
            continue;
        }

        if (bit_is_one) {
            consecutive_ones++;
            data_bits |= (1u << data_bit_count);
        } else {
            consecutive_ones = 0;
        }
        data_bit_count++;
    }

    if (data_bit_count < 8) {
        return 0;
    }
    return (uint8_t) data_bits;
}

sniffer_result_t decode(const uint32_t* buf, uint32_t n_words, uint32_t mark_samples, bool& toggle_seen) {
    sniffer_result_t result = {};
    toggle_seen = false;

    uint32_t const n_samples = n_words * kSamplesPerWord;
    if (n_samples < 4) {
        return result;
    }

    // word_idx and phase track i / kSamplesPerWord and i % kSamplesPerWord
    // incrementally, since division is slow on Cortex-M0+.
    bool have_button_edge = false;
    uint32_t button_edge = 0;
    bool const prev_button = get_button(get_sample(buf, 0));
    {
        uint32_t i = 1;
        uint32_t word_idx = 0;
        uint32_t phase = 1;
        while (i < n_samples) {
            if (phase == 0 && i + kSamplesPerWord <= n_samples &&
                word_all_button_state(buf, word_idx, prev_button)) {
                i += kSamplesPerWord;
                word_idx++;
                continue;
            }
            if (get_button(get_sample(buf, i)) != prev_button) {
                button_edge = i;
                have_button_edge = true;
                break;
            }
            i++;
            phase++;
            if (phase == kSamplesPerWord) {
                phase = 0;
                word_idx++;
            }
        }
    }
    if (!have_button_edge) {
        return result;
    }
    toggle_seen = true;

    constexpr uint32_t kIdleDebounce = 2;
    constexpr uint32_t kEopDebounce = 2;
    constexpr uint32_t kMaxIntervals = 64;
    PacketInterval intervals[kMaxIntervals];
    uint32_t n_intervals = 0;

    uint32_t i = 0;
    uint32_t word_idx = 0;
    uint32_t phase = 0;
    auto advance_i = [&](uint32_t delta) {
        i += delta;
        phase += delta;
        if (phase >= kSamplesPerWord) {
            phase -= kSamplesPerWord;
            word_idx++;
        }
    };
    while (n_intervals < kMaxIntervals) {
        bool found_start = false;
        while (i + kIdleDebounce <= n_samples) {
            if (phase == 0 && i + kSamplesPerWord <= n_samples && word_is_all_idle(buf, word_idx)) {
                advance_i(kSamplesPerWord);
                continue;
            }
            bool busy = true;
            for (uint32_t k = 0; k < kIdleDebounce; k++) {
                if (classify(decode_line(get_sample(buf, i + k))) == LineSymbol::J) {
                    busy = false;
                    break;
                }
            }
            if (busy) {
                found_start = true;
                break;
            }
            advance_i(1);
        }
        if (!found_start) {
            break;
        }

        uint32_t const start = i;

        bool found_eop = false;
        while (i + kEopDebounce <= n_samples) {
            bool eop = true;
            for (uint32_t k = 0; k < kEopDebounce; k++) {
                if (classify(decode_line(get_sample(buf, i + k))) != LineSymbol::SE0) {
                    eop = false;
                    break;
                }
            }
            if (eop) {
                found_eop = true;
                break;
            }
            advance_i(1);
        }
        if (!found_eop) {
            break;  // truncated by the end of the capture
        }

        uint32_t const end = i;
        // Skip glitches; even the shortest real packet is much longer.
        constexpr uint32_t kMinIntervalSamples = 20;
        if (end - start >= kMinIntervalSamples) {
            uint8_t const pid = decode_pid(buf, start, end, samples_per_bit_fx);
            intervals[n_intervals++] = { start, end, pid };
        }
        advance_i(kEopDebounce);
    }

    auto to_ns = [](int64_t samples) { return (int32_t) (samples * 1000000000 / (int64_t) sample_hz); };

    int pre_toggle_sof_idx = -1;
    for (uint32_t k = 0; k < n_intervals; k++) {
        if (intervals[k].start > button_edge) {
            break;
        }
        if (intervals[k].pid == kPidSof) {
            pre_toggle_sof_idx = (int) k;
        }
    }
    if (pre_toggle_sof_idx >= 0) {
        result.toggle_offset_valid = true;
        result.toggle_offset_in_frame_ns =
            to_ns((int64_t) button_edge - (int64_t) intervals[pre_toggle_sof_idx].start);
    }

    int post_toggle_sof_idx = -1;
    for (uint32_t k = 0; k < n_intervals; k++) {
        if (intervals[k].pid == kPidSof && intervals[k].start > button_edge) {
            post_toggle_sof_idx = (int) k;
            break;
        }
    }

    if (pre_toggle_sof_idx >= 0 && post_toggle_sof_idx >= 0) {
        result.sof_gap_valid = true;
        result.sof_gap_ns =
            to_ns((int64_t) intervals[post_toggle_sof_idx].start - (int64_t) intervals[pre_toggle_sof_idx].start);
    }

    // Logs every decoded packet, with times relative to the button edge.
    auto dump_intervals = [&](const char* why) {
        printf("# sniffer: %s (button_edge=%lu mark=%lu n_samples=%lu n_intervals=%lu)\n", why,
            (unsigned long) button_edge, (unsigned long) mark_samples, (unsigned long) n_samples,
            (unsigned long) n_intervals);
        for (uint32_t k = 0; k < n_intervals; k++) {
            printf("# sniffer:   pid=%02x start=%ld end=%ld\n", intervals[k].pid,
                (long) to_ns((int64_t) intervals[k].start - (int64_t) button_edge),
                (long) to_ns((int64_t) intervals[k].end - (int64_t) button_edge));
        }
    };

    int resp_idx = -1;
    for (int k = (int) n_intervals - 1; k >= 0; k--) {
        if (intervals[k].start > mark_samples) {
            continue;
        }
        if (intervals[k].pid == kPidData0 || intervals[k].pid == kPidData1) {
            resp_idx = k;
            break;
        }
    }
    if (resp_idx < 0) {
        dump_intervals("no DATA0/DATA1 before the report was seen");
        return result;
    }

    int in_idx = -1;
    for (int k = resp_idx - 1; k >= 0; k--) {
        if (intervals[k].pid == kPidIn) {
            in_idx = k;
            break;
        }
    }
    int sof_idx = -1;
    if (in_idx >= 0) {
        for (int k = in_idx - 1; k >= 0; k--) {
            if (intervals[k].pid == kPidSof) {
                sof_idx = k;
                break;
            }
        }
    }
    if (in_idx < 0 || sof_idx < 0 || post_toggle_sof_idx < 0) {
        dump_intervals("response has no IN/SOF/post-toggle SOF to pair with");
        return result;
    }

    result.valid = true;
    result.sof_to_in_ns = to_ns((int64_t) intervals[in_idx].start - (int64_t) intervals[sof_idx].start);
    result.toggle_to_response_start_ns = to_ns((int64_t) intervals[resp_idx].start - (int64_t) button_edge);
    result.response_start_to_end_ns = to_ns((int64_t) intervals[resp_idx].end - (int64_t) intervals[resp_idx].start);
    result.next_sof_to_response_end_ns =
        to_ns((int64_t) intervals[resp_idx].end - (int64_t) intervals[post_toggle_sof_idx].start);
    return result;
}

sniffer_failure_t classify_failure(const sniffer_result_t& result, bool toggle_seen, bool input_after_window) {
    if (result.valid && result.toggle_offset_valid && result.sof_gap_valid) {
        return sniffer_failure_t::NONE;
    }
    if (input_after_window) {
        return sniffer_failure_t::RESPONSE_OUTSIDE_WINDOW;
    }
    if (!toggle_seen) {
        return sniffer_failure_t::TOGGLE_NOT_CAPTURED;
    }
    if (!result.toggle_offset_valid || !result.sof_gap_valid) {
        return sniffer_failure_t::SOF_NOT_CAPTURED;
    }
    return sniffer_failure_t::RESPONSE_NOT_DECODED;
}

}  // namespace

void sniffer_init() {
    sniffer_sm = pio_claim_unused_sm(sniffer_pio, true);
    uint const offset = pio_add_program(sniffer_pio, &event_sniffer_program);

    pio_sm_config c = event_sniffer_program_get_default_config(offset);
    sm_config_set_in_pins(&c, kSampleBase);
    sm_config_set_in_shift(&c, true, true, kAutopushThreshold);
    sm_config_set_clkdiv(&c, (float) kClkDiv);

    pio_sm_set_consecutive_pindirs(sniffer_pio, sniffer_sm, kSampleBase, 3, false);
    pio_sm_init(sniffer_pio, sniffer_sm, offset, &c);

    sample_hz = clock_get_hz(clk_sys) / kClkDiv;
    samples_per_bit_fx = (uint32_t) (((uint64_t) sample_hz << kBitFracBits) / 12000000);
    capture_words = (uint32_t) (((uint64_t) sample_hz * kCaptureTotalUs) / 1000000ULL / kSamplesPerWord);
    if (capture_words > kMaxCaptureWords) {
        capture_words = kMaxCaptureWords;
    }

    dma_chan = dma_claim_unused_channel(true);
}

void sniffer_arm() {
    mark_words = capture_words;

    pio_sm_set_enabled(sniffer_pio, sniffer_sm, false);
    pio_sm_clear_fifos(sniffer_pio, sniffer_sm);
    pio_sm_restart(sniffer_pio, sniffer_sm);

    dma_channel_abort(dma_chan);
    dma_channel_config dc = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(sniffer_pio, sniffer_sm, false));
    dma_channel_configure(dma_chan, &dc, capture_buffer, &sniffer_pio->rxf[sniffer_sm], capture_words, true);

    pio_sm_set_enabled(sniffer_pio, sniffer_sm, true);
}

void sniffer_mark_input() {
    uint32_t const remaining = dma_channel_hw_addr(dma_chan)->transfer_count;
    mark_words = capture_words - remaining;
}

sniffer_result_t sniffer_collect() {
    pio_sm_set_enabled(sniffer_pio, sniffer_sm, false);
    uint32_t const remaining = dma_channel_hw_addr(dma_chan)->transfer_count;
    dma_channel_abort(dma_chan);
    uint32_t const words_captured = capture_words - remaining;
    uint32_t const mark = (mark_words < words_captured) ? mark_words : words_captured;

    // Only decode up to a frame past the mark, which is enough to include the
    // SOF after the toggle, to save decode time.
    uint32_t const margin_words = (uint32_t) (((uint64_t) sample_hz * 1500) / 1000000ULL / kSamplesPerWord) + 1;
    uint32_t n_words = mark + margin_words;
    if (n_words > words_captured) {
        n_words = words_captured;
    }

    bool toggle_seen;
    sniffer_result_t result = decode(capture_buffer, n_words, mark * kSamplesPerWord, toggle_seen);

    bool const input_after_window = (mark_words >= capture_words);
    result.failure = classify_failure(result, toggle_seen, input_after_window);
    return result;
}
