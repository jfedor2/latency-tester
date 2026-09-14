// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "bsp/board_api.h"
#include "tusb.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/stdio.h"
#include "pico/time.h"

#if (CFG_TUH_ENABLED && CFG_TUH_RPI_PIO_USB) || (CFG_TUD_ENABLED && CFG_TUD_RPI_PIO_USB)
#include "pio_usb.h"
#endif

#include "callbacks.h"
#include "descriptor_parser.h"
#include "git_describe.h"
#include "json_format.h"
#include "sniffer.h"
#include "tester_pins.h"
#include "toggle.h"

#define BUILD_ID BUILD_BOARD "/" BUILD_VARIANT "/" GIT_DESCRIBE

namespace {

constexpr uint32_t kTotalSamples = 2048;

volatile bool device_connected = false;
volatile bool input_happened = false;
volatile uint16_t current_vid = 0;
volatile uint16_t current_pid = 0;
// Quoted JSON strings, empty if the device has no such string.
char current_manufacturer_json[160] = "";
char current_product_json[160] = "";

// Matches the descriptor parser's limit on bit positions.
constexpr size_t kMaxReportBytes = 64;

struct ReportState {
    uint8_t relevant_mask[kMaxReportBytes];  // mask of the button/hat bits
    uint8_t previous[kMaxReportBytes];
};

struct InterfaceState {
    bool has_report_id = false;
    // Only reports that have buttons or a hat in them, by report ID.
    std::unordered_map<uint8_t, ReportState> reports;
};

// By interface_key(). Only touched from core 0.
std::unordered_map<uint16_t, InterfaceState> interfaces;

uint16_t interface_key(uint8_t dev_addr, uint8_t instance) {
    return (uint16_t) (dev_addr << 8) | instance;
}

inline void put_bit(uint8_t* data, int len, uint16_t bitpos, uint8_t value) {
    int byte_no = bitpos / 8;
    int bit_no = bitpos % 8;
    if (byte_no < len) {
        data[byte_no] &= ~(1 << bit_no);
        data[byte_no] |= (value & 1) << bit_no;
    }
}

inline void put_bits(uint8_t* data, int len, uint16_t bitpos, uint8_t size, uint32_t value) {
    for (int i = 0; i < size; i++) {
        put_bit(data, len, bitpos + i, (value >> i) & 1);
    }
}

// Any interrupt IN endpoint has bInterval other than 1, so a capture window
// might not contain a poll.
bool slow_polling_endpoint = false;

void announce_device(uint8_t dev_addr) {
    uint16_t vid;
    uint16_t pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    current_vid = vid;
    current_pid = pid;

    // Read from core 1 without locking; device_connected is false while
    // they're written.
    current_manufacturer_json[0] = '\0';
    current_product_json[0] = '\0';
    uint16_t langid = 0;
    {
        uint8_t buf[4];
        if (tuh_descriptor_get_string_langid_sync(dev_addr, buf, sizeof(buf)) == XFER_RESULT_SUCCESS) {
            langid = (uint16_t) buf[2] | ((uint16_t) buf[3] << 8);
        }
    }
    if (langid != 0) {
        uint8_t buf[128];
        if (tuh_descriptor_get_manufacturer_string_sync(dev_addr, langid, buf, sizeof(buf)) == XFER_RESULT_SUCCESS) {
            size_t p = 0;
            append_usb_string_descriptor_as_json(current_manufacturer_json, sizeof(current_manufacturer_json), p, buf);
            current_manufacturer_json[p] = '\0';
        }
        if (tuh_descriptor_get_product_string_sync(dev_addr, langid, buf, sizeof(buf)) == XFER_RESULT_SUCCESS) {
            size_t p = 0;
            append_usb_string_descriptor_as_json(current_product_json, sizeof(current_product_json), p, buf);
            current_product_json[p] = '\0';
        }
    }

    char line[400];
    size_t pos = 0;
    append_str(line, sizeof(line), pos, "#[\"device_connected\",{");
    append_device_fields_json(line, sizeof(line), pos, vid, pid, current_manufacturer_json, current_product_json);
    append_str(line, sizeof(line), pos, "}]");
    line[pos] = '\0';
    printf("%s\n", line);

    if (slow_polling_endpoint) {
        slow_polling_endpoint = false;
        printf("# warning: bInterval is not 1\n");
    }
}

// One step of the splitmix32 PRNG.
uint32_t splitmix32(uint32_t& state) {
    state += 0x9E3779B9u;
    uint32_t z = state;
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

// The murmur3 finalizer.
uint32_t fmix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    return h ^ (h >> 16);
}

// A pseudo-random permutation of [0, size), computed per element without
// storage. A Feistel network over the next even power of two, with cycle
// walking to skip out-of-range values.
class RandomPermutation {
public:
    RandomPermutation(uint32_t seed, uint32_t size)
        : size_(size), half_(((size > 1 ? 32 - __builtin_clz(size - 1) : 1) + 1) / 2) {
        for (uint32_t& key : keys_) {
            key = splitmix32(seed);
        }
    }

    uint32_t operator()(uint32_t i) const {
        uint32_t x = encrypt(i);
        while (x >= size_) {
            x = encrypt(x);
        }
        return x;
    }

private:
    static constexpr int kRounds = 4;

    uint32_t encrypt(uint32_t x) const {
        uint32_t left = x >> half_;
        uint32_t right = x & ((1u << half_) - 1);
        for (uint32_t key : keys_) {
            uint32_t const next_right = left ^ (fmix32(right ^ key) >> (32 - half_));
            left = right;
            right = next_right;
        }
        return (left << half_) | right;
    }

    uint32_t size_;
    uint32_t half_;
    uint32_t keys_[kRounds];
};

const char* failure_reason(sniffer_failure_t failure) {
    switch (failure) {
        case sniffer_failure_t::RESPONSE_OUTSIDE_WINDOW:
            return "response outside capture window";
        case sniffer_failure_t::TOGGLE_NOT_CAPTURED:
            return "toggle not captured";
        case sniffer_failure_t::SOF_NOT_CAPTURED:
            return "SOF not captured";
        case sniffer_failure_t::RESPONSE_NOT_DECODED:
        default:
            return "response not decoded";
    }
}

// Can outlast the test: the last round's release is still awaited after it ends.
enum class Phase {
    IDLE,
    AWAITING_INPUT,
    // Every measured toggle is a press, so after each one the button is
    // released and the DUT has to report that before the next round.
    AWAITING_RELEASE_ACK,
};

void core1_entry() {
    Phase phase = Phase::IDLE;
    uint64_t round_armed_at_us = 0;
    uint64_t reset_at_us = 0;
    uint32_t samples_left = 0;
    uint32_t sample_index = 0;
    uint32_t round_sample_index = 0;
    uint64_t total_latency = 0;
    uint32_t valid_samples = 0;
    bool test_pending = false;
    // From "starting_test" to "test_finished"/"test_aborted".
    bool test_running = false;
    // Not acted on while a round is armed, to not leave the toggle in an
    // unknown state.
    bool abort_requested = false;
    int c;

    // Doesn't touch the phase or the button.
    auto abort_test = [&](const char* reason) {
        samples_left = 0;
        test_running = false;
        board_led_write(false);
        printf("#[\"test_aborted\",{\"reason\":\"%s\"}]\n", reason);
    };

#if !CFG_TUH_RPI_PIO_USB
    // Pico-PIO-USB does this on the PIO build. The sniffer and toggle expect
    // inverted inputs, and RP2350 pads need gpio_init() to enable input.
    gpio_init(kDpPin);
    gpio_init(kDpPin + 1);
    gpio_set_inover(kDpPin, GPIO_OVERRIDE_INVERT);
    gpio_set_inover(kDpPin + 1, GPIO_OVERRIDE_INVERT);
#endif

    sniffer_init();
    toggle_init();

    uint32_t const cycles_per_frame = clock_get_hz(clk_sys) / 1000;

    // The order the within-frame toggle offsets are visited in. The same
    // for every test.
    static_assert((kTotalSamples & (kTotalSamples - 1)) == 0, "kTotalSamples must be a power of two");
    constexpr uint32_t kOffsetBits = __builtin_ctz(kTotalSamples);
    constexpr uint32_t kOffsetOrderSeed = 0x6C617465;
    RandomPermutation const offset_order(kOffsetOrderSeed, kTotalSamples);

    printf("# Hello.\n");

    uint64_t next_round_target_us = 0;

    while (true) {
        uint64_t now = time_us_64();

        if (test_running && !device_connected) {
            if (phase == Phase::AWAITING_INPUT) {
                sniffer_collect();
            }
            input_happened = false;
            toggle_abort();
            phase = Phase::IDLE;
            abort_requested = false;
            abort_test("device disconnected");
        } else if (abort_requested && phase != Phase::AWAITING_INPUT) {
            abort_requested = false;
            if (test_running) {
                abort_test("requested by user");
            }
        } else if (samples_left > 0 && phase == Phase::IDLE) {
            samples_left--;
            round_sample_index = sample_index;
            // Each of kTotalSamples evenly spaced offsets is visited once.
            uint32_t const wait_cycles =
                (uint32_t) (((uint64_t) offset_order(sample_index) * cycles_per_frame) >> kOffsetBits);
            sample_index++;

            // Space rounds out so they don't sync up with the device's own
            // periodic processing or debouncing.
            constexpr uint64_t kRoundIntervalUs = 29000;
            if (next_round_target_us == 0) {
                // Align the grid to mid-frame, so arming never races an SOF
                // and round spacing doesn't vary by a frame.
                constexpr uint32_t kGridOffsetUs = 500;
                if (!toggle_sync_to_sof((uint64_t) cycles_per_frame * kGridOffsetUs / 1000, 5000)) {
                    printf("# warning: no SOF to align the round grid to\n");
                }
                next_round_target_us = time_us_64();
            }
            // Plus a random number of whole frames, which keeps the SOF alignment.
            constexpr uint32_t kMaxExtraFrames = 60;
            constexpr uint32_t kExtraFramesSeed = 0x6772696E;
            uint32_t const extra_frames = RandomPermutation(
                kExtraFramesSeed + round_sample_index / kMaxExtraFrames, kMaxExtraFrames)(
                round_sample_index % kMaxExtraFrames);
            next_round_target_us += kRoundIntervalUs + extra_frames * 1000ULL;
            uint64_t const now_for_sched = time_us_64();
            if (now_for_sched > next_round_target_us) {
                uint64_t const overrun_us = now_for_sched - next_round_target_us;
                printf("# warning: round overran its %llu us target by %llu us\n",
                    (unsigned long long) kRoundIntervalUs, (unsigned long long) overrun_us);
                // Skip to the next future grid point instead of catching up.
                next_round_target_us += kRoundIntervalUs * (overrun_us / kRoundIntervalUs + 1);
            }
            sleep_until(from_us_since_boot(next_round_target_us));

            input_happened = false;
            sniffer_arm();
            toggle_arm(wait_cycles);
            round_armed_at_us = time_us_64();
            phase = Phase::AWAITING_INPUT;
        }

        if (phase == Phase::AWAITING_INPUT && input_happened) {
            sniffer_mark_input();

            // Keep capturing until the SOF after the toggle.
            sleep_us(1200);

            sniffer_result_t const sniff = sniffer_collect();

            if (sniff.failure == sniffer_failure_t::NONE) {
                total_latency +=
                    (uint64_t) sniff.toggle_to_response_start_ns + (uint64_t) sniff.response_start_to_end_ns;
                valid_samples++;
                printf("%lu %ld %ld %ld %ld %ld %ld\n", (unsigned long) round_sample_index,
                    (long) sniff.toggle_offset_in_frame_ns, (long) sniff.next_sof_to_response_end_ns,
                    (long) sniff.toggle_to_response_start_ns, (long) sniff.sof_to_in_ns,
                    (long) sniff.response_start_to_end_ns, (long) sniff.sof_gap_ns);
            } else {
                printf(
                    "# failed sample %lu (%s): toggle_offset_valid=%d sof_to_toggle_ns=%ld valid=%d "
                    "next_sof_to_response_end_ns=%ld toggle_to_response_ns=%ld sof_to_in_ns=%ld "
                    "response_duration_ns=%ld sof_gap_valid=%d sof_gap_ns=%ld\n",
                    (unsigned long) round_sample_index, failure_reason(sniff.failure), sniff.toggle_offset_valid,
                    (long) sniff.toggle_offset_in_frame_ns, sniff.valid, (long) sniff.next_sof_to_response_end_ns,
                    (long) sniff.toggle_to_response_start_ns, (long) sniff.sof_to_in_ns,
                    (long) sniff.response_start_to_end_ns, sniff.sof_gap_valid, (long) sniff.sof_gap_ns);
            }

            input_happened = false;
            toggle_reset();
            phase = Phase::AWAITING_RELEASE_ACK;
            reset_at_us = time_us_64();

            if (sniff.failure != sniffer_failure_t::NONE) {
                abort_test(failure_reason(sniff.failure));
            } else if (samples_left == 0) {
                test_running = false;
                board_led_write(false);
                printf("#[\"test_finished\",{\"average_latency_ns\":%llu}]\n",
                    (unsigned long long) (total_latency / valid_samples));
            }
        }

        if (phase == Phase::AWAITING_INPUT && (now > round_armed_at_us + 500000)) {
            sniffer_collect();
            // The toggle may not have fired, so toggle_reset() isn't safe.
            input_happened = false;
            toggle_abort();
            phase = Phase::AWAITING_RELEASE_ACK;
            reset_at_us = time_us_64();

            if (test_running) {
                abort_test("input dropped");
            }
        }

        if (phase == Phase::AWAITING_RELEASE_ACK && input_happened) {
            input_happened = false;
            phase = Phase::IDLE;
        }

        if (phase == Phase::AWAITING_RELEASE_ACK && (now > reset_at_us + 500000)) {
            phase = Phase::IDLE;
            if (test_running) {
                abort_test("release not acknowledged");
            }
        }

        if (test_pending && device_connected) {
            test_pending = false;
            test_running = true;
            abort_requested = false;
            total_latency = 0;
            valid_samples = 0;
            samples_left = kTotalSamples;
            sample_index = 0;
            next_round_target_us = 0;
            board_led_write(true);
            printf("#[\"starting_test\",{\"total_samples\":%lu}]\n", (unsigned long) kTotalSamples);
            printf(
                "# sample_index sof_to_toggle_ns next_sof_to_response_end_ns toggle_to_response_ns sof_to_in_ns "
                "response_duration_ns sof_gap_ns\n");
        }

        if ((c = getchar_timeout_us(0)) >= 0) {
            switch (c) {
                case 't':
                    if (!device_connected) {
                        printf("# warning: no device connected\n");
                    } else if (!test_running) {
                        test_pending = true;
                    }
                    break;
                case 'x':
                    if (test_running) {
                        abort_requested = true;
                    } else {
                        printf("#[\"test_not_running\",{}]\n");
                    }
                    break;
                case 's': {
                    char line[400];
                    size_t pos = 0;
                    if (device_connected) {
                        append_str(line, sizeof(line), pos, "#[\"status\",{\"connected\":true,");
                        append_device_fields_json(line, sizeof(line), pos, current_vid, current_pid, current_manufacturer_json, current_product_json);
                        append_str(line, sizeof(line), pos, ",\"build_id\":\"" BUILD_ID "\"}]");
                    } else {
                        append_str(line, sizeof(line), pos, "#[\"status\",{\"connected\":false,\"build_id\":\"" BUILD_ID "\"}]");
                    }
                    line[pos] = '\0';
                    printf("%s\n", line);
                    break;
                }
            }
        }
    }
}

void our_board_init() {
    // A multiple of 12 MHz, for Pico-PIO-USB.
    set_sys_clock_khz(156000, true);

#if (CFG_TUH_ENABLED && CFG_TUH_RPI_PIO_USB) || (CFG_TUD_ENABLED && CFG_TUD_RPI_PIO_USB)

#ifdef PICO_DEFAULT_PIO_USB_VBUSEN_PIN
    gpio_init(PICO_DEFAULT_PIO_USB_VBUSEN_PIN);
    gpio_set_dir(PICO_DEFAULT_PIO_USB_VBUSEN_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_PIO_USB_VBUSEN_PIN, PICO_DEFAULT_PIO_USB_VBUSEN_STATE);
#endif

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = PICO_DEFAULT_PIO_USB_DP_PIN;
    tuh_configure(BOARD_TUH_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
#endif

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
}

}  // namespace

int main() {
    our_board_init();
    tusb_init();
    stdio_init_all();

#if CFG_TUH_RPI_PIO_USB
    // Pico-PIO-USB's host runs in the hardware alarm 2 ISR. Let it preempt
    // the native USB device IRQ so SOF/IN packets aren't delayed.
    irq_set_priority(hardware_alarm_get_irq_num(2), PICO_HIGHEST_IRQ_PRIORITY);
#endif

    multicore_launch_core1(core1_entry);

    while (1) {
        tuh_task();
#if CFG_TUH_RPI_PIO_USB
        // stdio_usb doesn't run tud_task() for us since we link tinyusb directly.
        tud_task();
#endif
    }

    return 0;
}

void descriptor_received_callback(uint8_t dev_addr, uint8_t instance, const uint8_t* report_descriptor, int len) {
    bool const first_interface = interfaces.empty();
    if (first_interface) {
        announce_device(dev_addr);
    }

    std::unordered_map<uint8_t, std::unordered_map<uint32_t, usage_def_t>> input_usages;
    std::unordered_map<uint8_t, std::unordered_map<uint32_t, usage_def_t>> output_usages;
    std::unordered_map<uint8_t, std::unordered_map<uint32_t, usage_def_t>> feature_usages;

    InterfaceState& itf = interfaces[interface_key(dev_addr, instance)];
    itf = InterfaceState{};

    parse_descriptor(
        input_usages,
        output_usages,
        feature_usages,
        itf.has_report_id,
        report_descriptor,
        len);

    for (auto const& [report_id, usage_map] : input_usages) {
        for (auto const& [usage, usage_def] : usage_map) {
            if (((usage >> 16) == 0x0009) || (usage == 0x00010039)) {
                put_bits(itf.reports[report_id].relevant_mask, kMaxReportBytes, usage_def.bitpos, usage_def.size, 0xFFFFFFFF);
            }
        }
    }

    printf("# relevant_mask (dev %u, interface %u):\n", dev_addr, instance);
    for (auto const& [report_id, report] : itf.reports) {
        printf("# (%d) ", report_id);
        for (size_t i = 0; i < kMaxReportBytes; i++) {
            printf("%02x", report.relevant_mask[i]);
        }
        printf("\n");
    }

    if (first_interface) {
        device_connected = true;
    }
}

bool tuh_enum_descriptor_configuration_cb(uint8_t daddr, uint8_t cfg_index, const tusb_desc_configuration_t* desc_config) {
    (void) daddr;
    (void) cfg_index;
    uint8_t const* const desc = (uint8_t const*) desc_config;
    uint16_t const total = tu_le16toh(desc_config->wTotalLength);
    slow_polling_endpoint = false;
    for (uint16_t pos = 0; pos + 2 <= total && desc[pos] != 0; pos += desc[pos]) {
        if (desc[pos + 1] != TUSB_DESC_ENDPOINT || pos + sizeof(tusb_desc_endpoint_t) > total) {
            continue;
        }
        auto const* ep = (tusb_desc_endpoint_t const*) &desc[pos];
        if (ep->bmAttributes.xfer == TUSB_XFER_INTERRUPT && tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN &&
            ep->bInterval != 1) {
            slow_polling_endpoint = true;
        }
    }
    return true;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len) {
    descriptor_received_callback(dev_addr, instance, desc_report, desc_len);
    tuh_hid_receive_report(dev_addr, instance);
}

void umount_callback(uint8_t dev_addr, uint8_t instance) {
    if (interfaces.erase(interface_key(dev_addr, instance)) == 0) {
        return;
    }
    if (interfaces.empty()) {
        printf("#[\"device_disconnected\",{}]\n");
        device_connected = false;
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    umount_callback(dev_addr, instance);
}

void report_received_callback(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
    if (len == 0) {
        return;
    }

    auto const itf_it = interfaces.find(interface_key(dev_addr, instance));
    if (itf_it == interfaces.end()) {
        return;
    }
    InterfaceState& itf = itf_it->second;

    uint8_t report_id = 0;
    if (itf.has_report_id) {
        report_id = report[0];
        report++;
        len--;
    }

    auto const report_it = itf.reports.find(report_id);
    if (report_it == itf.reports.end()) {
        return;
    }
    ReportState& state = report_it->second;

    if (len > kMaxReportBytes) {
        len = kMaxReportBytes;
    }

    for (uint16_t i = 0; i < len; i++) {
        if ((report[i] & state.relevant_mask[i]) != (state.previous[i] & state.relevant_mask[i])) {
            input_happened = true;
            break;
        }
    }

    memcpy(state.previous, report, len);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
    report_received_callback(dev_addr, instance, report, len);
    tuh_hid_receive_report(dev_addr, instance);
}
