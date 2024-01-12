#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bsp/board_api.h"
#include "tusb.h"

#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/stdio.h"

#include "callbacks.h"
#include "descriptor_parser.h"

#define BUTTON_PIN 2
#define TOTAL_SAMPLES 3000

volatile bool device_connected = false;
volatile uint64_t last_sof_us = 0;
volatile bool sof_happened = false;
volatile uint32_t samples_left = 0;
volatile bool input_happened = false;
volatile uint64_t total_latency = 0;

bool has_report_id;
std::unordered_map<uint8_t, uint8_t[64]> relevant_bits;

void core1_entry() {
    uint32_t us_within_frame = 0;
    uint32_t waiting_for_input = false;
    uint64_t toggle_button_at_us = 0;
    bool button_state = true;
    bool toggle_scheduled = false;

    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_OUT);
    gpio_put(BUTTON_PIN, button_state);

    printf("# Hello.\n");

    while (1) {
        uint64_t now = time_us_64();

        if (sof_happened) {
            sof_happened = false;
            if (device_connected && (samples_left > 0) && !waiting_for_input && !toggle_scheduled) {
                if (--samples_left == 0) {
                    board_led_write(false);
                }
                us_within_frame = samples_left % 1000;
                toggle_button_at_us = last_sof_us + 10000 + 1000 * (samples_left % 10) + us_within_frame;
                printf("%lu ", us_within_frame);
                toggle_scheduled = true;
            }
        }

        if (toggle_scheduled && (now > toggle_button_at_us)) {
            button_state = !button_state;
            gpio_put(BUTTON_PIN, button_state);
            toggle_scheduled = false;
            waiting_for_input = true;
            input_happened = false;
        }

        if (waiting_for_input && input_happened) {
            total_latency += now - toggle_button_at_us;
            printf("%llu\n", now - toggle_button_at_us - 1000 + us_within_frame);
            input_happened = false;
            waiting_for_input = false;
            if (samples_left == 0) {
                printf("# average latency: %lluus\n", total_latency / TOTAL_SAMPLES);
            }
        }

        if (waiting_for_input && (now > toggle_button_at_us + 500000)) {
            printf("input dropped\n");
            waiting_for_input = false;
        }
    }
}

int main() {
    board_init();
    tusb_init();
    stdio_init_all();

    multicore_launch_core1(core1_entry);

    while (1) {
        tuh_task();
    }

    return 0;
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

void descriptor_received_callback(uint16_t vendor_id, uint16_t product_id, const uint8_t* report_descriptor, int len, uint16_t interface) {
    std::unordered_map<uint8_t, std::unordered_map<uint32_t, usage_def_t>> input_usages;
    std::unordered_map<uint8_t, std::unordered_map<uint32_t, usage_def_t>> output_usages;
    std::unordered_map<uint8_t, std::unordered_map<uint32_t, usage_def_t>> feature_usages;

    printf("# Device connected (%04x:%04x)\n", vendor_id, product_id);

    relevant_bits.clear();
    has_report_id = false;

    auto report_sizes_map = parse_descriptor(
        input_usages,
        output_usages,
        feature_usages,
        has_report_id,
        report_descriptor,
        len);

    for (auto const& [report_id, usage_map] : input_usages) {
        for (auto const& [usage, usage_def] : usage_map) {
            if (((usage >> 16) == 0x0009) || (usage == 0x00010039)) {
                put_bits(relevant_bits[report_id], 64, usage_def.bitpos, usage_def.size, 0xFFFFFFFF);
            }
        }
    }

    printf("# relevant_bits:\n");
    for (auto const& [report_id, relevant] : relevant_bits) {
        printf("# (%d) ", report_id);
        for (int i = 0; i < 64; i++) {
            printf("%02x", relevant[i]);
        }
        printf("\n");
    }

    total_latency = 0;
    samples_left = TOTAL_SAMPLES;
    device_connected = true;
    board_led_write(true);
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len) {
    uint16_t vid;
    uint16_t pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    descriptor_received_callback(vid, pid, desc_report, desc_len, (uint16_t) (dev_addr << 8) | instance);
    tuh_hid_receive_report(dev_addr, instance);
}

void umount_callback(uint8_t dev_addr, uint8_t instance) {
    printf("# Device disconnected.\n");
    device_connected = false;
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    umount_callback(dev_addr, instance);
}

void tuh_sof_cb() {
    last_sof_us = time_us_64();
    sof_happened = true;
}

void report_received_callback(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
    static uint8_t previous_report[64];  // XXX per-report_id

    if (len == 0) {
        return;
    }

    uint8_t report_id = 0;
    if (has_report_id) {
        report_id = report[0];
        report++;
        len--;
    }

    uint8_t* relevant_mask = relevant_bits[report_id];

    for (uint16_t i = 0; i < len; i++) {
        if ((report[i] & relevant_mask[i]) != (previous_report[i] & relevant_mask[i])) {
            input_happened = true;
            break;
        }
    }

    memcpy(previous_report, report, len);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
    report_received_callback(dev_addr, instance, report, len);
    tuh_hid_receive_report(dev_addr, instance);
}
