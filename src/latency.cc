#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bsp/board_api.h"
#include "tusb.h"

#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/stdio.h"

#include "callbacks.h"

#define BUTTON_PIN 2

volatile bool device_connected = false;
volatile uint64_t last_sof_us = 0;
volatile bool sof_happened = false;
volatile uint32_t samples_left = 0;
volatile bool input_happened = false;

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
            printf("%llu\n", now - toggle_button_at_us - 1000 + us_within_frame);
            input_happened = false;
            waiting_for_input = false;
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

void descriptor_received_callback(uint16_t vendor_id, uint16_t product_id, const uint8_t* report_descriptor, int len, uint16_t interface) {
    printf("# Device connected (%04x:%04x)\n", vendor_id, product_id);
    device_connected = true;
    samples_left = 3000;
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
    static uint8_t previous_report[64];

    if (memcmp(previous_report + 4, report + 4, 2)) {
        input_happened = true;
    }
    memcpy(previous_report, report, len);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
    report_received_callback(dev_addr, instance, report, len);
    tuh_hid_receive_report(dev_addr, instance);
}
