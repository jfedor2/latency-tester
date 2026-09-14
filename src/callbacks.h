// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#ifndef _CALLBACKS_H_
#define _CALLBACKS_H_

#include <stdint.h>

// `instance` is the HID instance, or the interface number for the Xbox driver.
void report_received_callback(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len);
void descriptor_received_callback(uint8_t dev_addr, uint8_t instance, const uint8_t* report_descriptor, int len);
void umount_callback(uint8_t dev_addr, uint8_t instance);

#endif
