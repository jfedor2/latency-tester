// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "json_format.h"

#include <stdio.h>

void append_char(char* out, size_t out_size, size_t& pos, char c) {
    if (pos + 1 < out_size) {
        out[pos++] = c;
    }
}

void append_str(char* out, size_t out_size, size_t& pos, const char* s) {
    for (; *s; s++) {
        append_char(out, out_size, pos, *s);
    }
}

void append_uint(char* out, size_t out_size, size_t& pos, unsigned value) {
    char buf[12];
    int const n = snprintf(buf, sizeof(buf), "%u", value);
    for (int i = 0; i < n; i++) {
        append_char(out, out_size, pos, buf[i]);
    }
}

namespace {

void append_json_escaped_codepoint(char* out, size_t out_size, size_t& pos, uint32_t cp) {
    if (cp == '"' || cp == '\\') {
        append_char(out, out_size, pos, '\\');
        append_char(out, out_size, pos, (char) cp);
    } else if (cp < 0x20) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\u%04x", (unsigned) cp);
        append_str(out, out_size, pos, buf);
    } else if (cp < 0x80) {
        append_char(out, out_size, pos, (char) cp);
    } else if (cp < 0x800) {
        append_char(out, out_size, pos, (char) (0xC0 | (cp >> 6)));
        append_char(out, out_size, pos, (char) (0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        append_char(out, out_size, pos, (char) (0xE0 | (cp >> 12)));
        append_char(out, out_size, pos, (char) (0x80 | ((cp >> 6) & 0x3F)));
        append_char(out, out_size, pos, (char) (0x80 | (cp & 0x3F)));
    } else {
        append_char(out, out_size, pos, (char) (0xF0 | (cp >> 18)));
        append_char(out, out_size, pos, (char) (0x80 | ((cp >> 12) & 0x3F)));
        append_char(out, out_size, pos, (char) (0x80 | ((cp >> 6) & 0x3F)));
        append_char(out, out_size, pos, (char) (0x80 | (cp & 0x3F)));
    }
}

}  // namespace

// Reads bytes rather than uint16_t since `desc` isn't guaranteed aligned.
void append_usb_string_descriptor_as_json(char* out, size_t out_size, size_t& pos, const uint8_t* desc) {
    append_char(out, out_size, pos, '"');
    uint8_t const blen = desc[0];
    if (blen >= 2) {
        uint32_t const n_units = (uint32_t) (blen - 2) / 2;
        for (uint32_t i = 0; i < n_units; i++) {
            uint32_t cp = (uint32_t) desc[2 + 2 * i] | ((uint32_t) desc[2 + 2 * i + 1] << 8);
            // Combine surrogate pairs; a lone surrogate becomes U+FFFD.
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n_units) {
                uint32_t const lo = (uint32_t) desc[2 + 2 * (i + 1)] | ((uint32_t) desc[2 + 2 * (i + 1) + 1] << 8);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i++;
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            append_json_escaped_codepoint(out, out_size, pos, cp);
        }
    }
    append_char(out, out_size, pos, '"');
}

// manufacturer_json/product_json are quoted JSON strings; empty ones are omitted.
void append_device_fields_json(char* out, size_t out_size, size_t& pos, uint16_t vid, uint16_t pid,
    const char* manufacturer_json, const char* product_json) {
    append_str(out, out_size, pos, "\"vid\":");
    append_uint(out, out_size, pos, vid);
    append_str(out, out_size, pos, ",\"pid\":");
    append_uint(out, out_size, pos, pid);
    if (manufacturer_json[0]) {
        append_str(out, out_size, pos, ",\"manufacturer\":");
        append_str(out, out_size, pos, manufacturer_json);
    }
    if (product_json[0]) {
        append_str(out, out_size, pos, ",\"product\":");
        append_str(out, out_size, pos, product_json);
    }
}
