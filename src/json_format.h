// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

// Helpers for building JSON lines in a fixed-size buffer. They truncate rather
// than overflow and leave room for, but don't write, the null terminator.

void append_char(char* out, size_t out_size, size_t& pos, char c);
void append_str(char* out, size_t out_size, size_t& pos, const char* s);
void append_uint(char* out, size_t out_size, size_t& pos, unsigned value);

// Appends a raw USB string descriptor as a quoted, escaped JSON string.
void append_usb_string_descriptor_as_json(char* out, size_t out_size, size_t& pos, const uint8_t* desc);

// vid/pid plus manufacturer/product (when non-empty), without the braces.
void append_device_fields_json(char* out, size_t out_size, size_t& pos, uint16_t vid, uint16_t pid,
    const char* manufacturer_json, const char* product_json);
