#ifndef _DESCRIPTOR_PARSER_H_
#define _DESCRIPTOR_PARSER_H_

#include <cstdint>
#include <unordered_map>

enum class ReportType : uint8_t {
    INPUT,
    OUTPUT,
    FEATURE,
};

struct usage_def_t {
    uint8_t report_id;
    uint8_t size;
    uint16_t bitpos;
    bool is_relative;
    bool is_array = false;
    int32_t logical_minimum;
    uint32_t index = 0;      // for arrays
    uint32_t count = 0;      // for arrays
    uint32_t usage_maximum;  // effective, for arrays/usage ranges
};

std::unordered_map<ReportType, std::unordered_map<uint8_t, uint16_t>> parse_descriptor(
    std::unordered_map<uint8_t, std::unordered_map<uint32_t, usage_def_t>>& input_usage_map,
    std::unordered_map<uint8_t, std::unordered_map<uint32_t, usage_def_t>>& output_usage_map,
    std::unordered_map<uint8_t, std::unordered_map<uint32_t, usage_def_t>>& feature_usage_map,
    bool& has_report_id,
    const uint8_t* report_descriptor,
    int len);

#endif
