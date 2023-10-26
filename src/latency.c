#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"

#include "hardware/gpio.h"
#include "pico/stdio.h"

#define BUTTON_PIN 2

bool device_connected = false;
uint64_t last_sof_us = 0;
bool sof_happened = false;
uint32_t samples_left = 0;
bool input_happened = false;
uint8_t received_report[64];
uint8_t previous_report[64];

int main() {
    uint32_t us_within_frame = 0;
    uint32_t waiting_for_input = false;
    uint64_t toggle_button_at_us = 0;
    bool button_state = true;
    bool toggle_scheduled = false;

    board_init();
    tusb_init();
    stdio_init_all();

    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_OUT);
    gpio_put(BUTTON_PIN, button_state);

    printf("Hello.\n");

    while (1) {
        tuh_task();  // <- all the callbacks are called here

        uint64_t now = time_us_64();

        if (sof_happened) {
            sof_happened = false;
            if (device_connected && (samples_left > 0) && !waiting_for_input && !toggle_scheduled) {
                samples_left--;
                us_within_frame = samples_left % 1000;
                toggle_button_at_us = last_sof_us + 10000 + us_within_frame;
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
            input_happened = false;
            if (memcmp(previous_report, received_report, 7)) {
                printf("%llu\n", now - toggle_button_at_us - 1000 + us_within_frame);
                waiting_for_input = false;
            }
        }

        if (waiting_for_input && (now > toggle_button_at_us + 500000)) {
            printf("input dropped\n");
            waiting_for_input = false;
        }
    }

    return 0;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len) {
    printf("Device connected.\n");
    device_connected = true;
    samples_left = 3000;
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    printf("Device disconnected.\n");
    device_connected = false;
}

void tuh_sof_cb() {
    last_sof_us = time_us_64();
    sof_happened = true;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
    input_happened = true;
    memcpy(previous_report, received_report, len);
    memcpy(received_report, report, len);
    tuh_hid_receive_report(dev_addr, instance);
}
