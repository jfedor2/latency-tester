// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#ifndef _SNIFFER_H_
#define _SNIFFER_H_

#include <cstdint>

// Passive PIO1 capture of D+, D- and the button pin, for timing the SOF, IN
// token and device response on a single clock.

// Claims a PIO1 state machine and a DMA channel.
void sniffer_init();

// Starts a capture window. Call before toggle_arm().
void sniffer_arm();

// In priority order: an earlier failure causes the later ones as a side effect.
enum class sniffer_failure_t : uint8_t {
    NONE,
    RESPONSE_OUTSIDE_WINDOW,  // the capture had already ended by the time the report was noticed
    TOGGLE_NOT_CAPTURED,      // no button edge in the capture
    SOF_NOT_CAPTURED,         // no SOF found before and/or after the toggle
    RESPONSE_NOT_DECODED,     // no response packet, or none that could be paired with an IN and SOF
};

struct sniffer_result_t {
    // NONE if and only if every field below is valid.
    sniffer_failure_t failure;

    bool valid;  // a DATA response was found and paired with its IN and SOF
    int32_t sof_to_in_ns;
    int32_t toggle_to_response_start_ns;
    int32_t response_start_to_end_ns;

    // From the SOF after the toggle to the end of the response. Negative if
    // the device answered within the toggle's own frame.
    int32_t next_sof_to_response_end_ns;

    // From the SOF before the toggle to the toggle.
    bool toggle_offset_valid;
    int32_t toggle_offset_in_frame_ns;

    // Between the SOFs before and after the toggle. Should be ~1000000ns.
    bool sof_gap_valid;
    int32_t sof_gap_ns;
};

// Records the capture position when the report was noticed, so the response
// can be told apart from later polls captured while waiting for the next SOF.
void sniffer_mark_input();

// Stops the capture and decodes it.
sniffer_result_t sniffer_collect();

#endif
