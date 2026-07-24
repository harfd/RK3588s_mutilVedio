#include "app_config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
using IniSection = std::map<std::string, std::string>;
using IniData = std::map<std::string, IniSection>;

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });
    return value;
}

bool parse_ini(const std::string& file_path, IniData& data, std::string& error) {
    std::ifstream input(file_path);
    if (!input.is_open()) {
        error = "cannot open config file: " + file_path;
        return false;
    }

    std::string section;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            if (section.empty()) {
                error = "empty section at line " + std::to_string(line_number);
                return false;
            }
            continue;
        }
        const auto delimiter = line.find('=');
        if (delimiter == std::string::npos || section.empty()) {
            error = "invalid config syntax at line " +
                    std::to_string(line_number);
            return false;
        }
        const std::string key = trim(line.substr(0, delimiter));
        const std::string value = trim(line.substr(delimiter + 1));
        if (key.empty()) {
            error = "empty key at line " + std::to_string(line_number);
            return false;
        }
        data[section][key] = value;
    }
    return true;
}

std::string get_string(const IniData& data, const std::string& section,
                       const std::string& key, const std::string& fallback = {}) {
    const auto section_it = data.find(section);
    if (section_it == data.end()) {
        return fallback;
    }
    const auto value_it = section_it->second.find(key);
    return value_it == section_it->second.end() ? fallback : value_it->second;
}

bool get_int(const IniData& data, const std::string& section,
             const std::string& key, int fallback, int& value,
             std::string& error) {
    const std::string raw = get_string(data, section, key);
    if (raw.empty()) {
        value = fallback;
        return true;
    }
    try {
        size_t parsed = 0;
        value = std::stoi(raw, &parsed);
        if (parsed != raw.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return true;
    } catch (...) {
        error = "invalid integer: [" + section + "] " + key + "=" + raw;
        return false;
    }
}

bool get_int_list(const IniData& data, const std::string& section,
                  const std::string& key, std::vector<int>& values,
                  std::string& error) {
    values.clear();
    const std::string raw = get_string(data, section, key);
    if (raw.empty()) {
        return true;
    }

    std::stringstream input(raw);
    std::string item;
    while (std::getline(input, item, ',')) {
        item = trim(item);
        if (item.empty()) {
            error = "invalid integer list: [" + section + "] " + key +
                    "=" + raw;
            return false;
        }
        try {
            size_t parsed = 0;
            const int value = std::stoi(item, &parsed);
            if (parsed != item.size()) {
                throw std::invalid_argument("trailing characters");
            }
            values.push_back(value);
        } catch (...) {
            error = "invalid integer list: [" + section + "] " + key +
                    "=" + raw;
            return false;
        }
    }
    if (values.empty()) {
        error = "invalid integer list: [" + section + "] " + key +
                "=" + raw;
        return false;
    }
    return true;
}

bool get_float(const IniData& data, const std::string& section,
               const std::string& key, float fallback, float& value,
               std::string& error) {
    const std::string raw = get_string(data, section, key);
    if (raw.empty()) {
        value = fallback;
        return true;
    }
    try {
        size_t parsed = 0;
        value = std::stof(raw, &parsed);
        if (parsed != raw.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return true;
    } catch (...) {
        error = "invalid number: [" + section + "] " + key + "=" + raw;
        return false;
    }
}

bool get_bool(const IniData& data, const std::string& section,
              const std::string& key, bool fallback, bool& value,
              std::string& error) {
    const std::string raw = lower(get_string(data, section, key));
    if (raw.empty()) {
        value = fallback;
        return true;
    }
    if (raw == "1" || raw == "true" || raw == "yes" || raw == "on") {
        value = true;
        return true;
    }
    if (raw == "0" || raw == "false" || raw == "no" || raw == "off") {
        value = false;
        return true;
    }
    error = "invalid boolean: [" + section + "] " + key + "=" + raw;
    return false;
}

bool has_scheme(const std::string& path) {
    return path.find("://") != std::string::npos;
}

bool is_absolute_path(const std::string& path) {
    return !path.empty() &&
           (path.front() == '/' || path.front() == '\\' ||
            (path.size() > 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
             path[1] == ':'));
}

std::string config_directory(const std::string& file_path) {
    const auto separator = file_path.find_last_of("/\\");
    return separator == std::string::npos ? "." : file_path.substr(0, separator);
}

std::string resolve_path(const std::string& base, const std::string& path) {
    if (path.empty() || is_absolute_path(path) || has_scheme(path)) {
        return path;
    }
    return base + "/" + path;
}

bool parse_source_type(const std::string& raw, InputSourceType& type) {
    const std::string value = lower(raw);
    if (value == "mp4" || value == "file") {
        type = InputSourceType::Mp4;
        return true;
    }
    if (value == "rtsp") {
        type = InputSourceType::Rtsp;
        return true;
    }
    if (value == "camera") {
        type = InputSourceType::Camera;
        return true;
    }
    return false;
}

bool in_unit_interval(float value) {
    return value >= 0.0f && value <= 1.0f;
}
}  // namespace

const char* input_source_type_name(InputSourceType type) {
    switch (type) {
        case InputSourceType::Mp4:
            return "mp4";
        case InputSourceType::Rtsp:
            return "rtsp";
        case InputSourceType::Camera:
            return "camera";
    }
    return "unknown";
}

bool AppConfigLoader::load(const std::string& file_path, AppConfig& config,
                           std::string& error) {
    IniData data;
    if (!parse_ini(file_path, data, error)) {
        return false;
    }
    if (data.find("global") == data.end() ||
        data.find("inference") == data.end() ||
        data.find("streaming") == data.end()) {
        error = "config requires [global], [inference] and [streaming] sections";
        return false;
    }

    if (!get_int(data, "global", "stream_count", 1,
                 config.global.stream_count, error) ||
        !get_int(data, "global", "infer_interval", 2,
                 config.global.infer_interval, error)) {
        return false;
    }
    if (config.global.stream_count < 1 || config.global.stream_count > 5) {
        error = "[global] stream_count must be between 1 and 5";
        return false;
    }
    if (config.global.infer_interval < 1) {
        error = "[global] infer_interval must be at least 1";
        return false;
    }

    const std::string base = config_directory(file_path);
    auto& inference = config.inference;
    inference.mode = lower(
        get_string(data, "inference", "mode", "legacy_multi"));
    inference.model_path = resolve_path(
        base, get_string(data, "inference", "model_path"));
    inference.label_path = resolve_path(
        base, get_string(data, "inference", "label_path"));
    inference.anchor_path = resolve_path(
        base, get_string(data, "inference", "anchor_path"));
    inference.person_model_path = resolve_path(
        base, get_string(data, "inference", "person_model_path"));
    inference.helmet_model_path = resolve_path(
        base, get_string(data, "inference", "helmet_model_path"));
    inference.callplay_model_path = resolve_path(
        base, get_string(data, "inference", "callplay_model_path"));
    if (inference.mode != "legacy_multi" &&
        inference.mode != "ppe_single") {
        error = "[inference] mode must be legacy_multi or ppe_single";
        return false;
    }
    if (inference.mode == "ppe_single" &&
        (inference.model_path.empty() ||
         inference.label_path.empty() ||
         inference.anchor_path.empty())) {
        error = "model_path, label_path and anchor_path are required for ppe_single";
        return false;
    }
    if (inference.mode == "legacy_multi" &&
        (inference.person_model_path.empty() ||
         inference.helmet_model_path.empty() ||
         inference.callplay_model_path.empty())) {
        error = "all three legacy model paths are required in [inference]";
        return false;
    }
    if (!get_int(data, "inference", "core", 0,
                 inference.core, error) ||
        !get_int(data, "inference", "class_count", 11,
                 inference.class_count, error) ||
        !get_int(data, "inference", "person_core", 0,
                  inference.person_core, error) ||
        !get_int(data, "inference", "helmet_core", 1,
                 inference.helmet_core, error) ||
        !get_int(data, "inference", "callplay_core", 2,
                 inference.callplay_core, error) ||
        !get_int(data, "inference", "person_class_count", 1,
                 inference.person_class_count, error) ||
        !get_int(data, "inference", "helmet_class_count", 2,
                 inference.helmet_class_count, error) ||
        !get_int(data, "inference", "callplay_class_count", 2,
                 inference.callplay_class_count, error) ||
        !get_float(data, "inference", "confidence_threshold", 0.25f,
                   inference.confidence_threshold, error) ||
        !get_float(data, "inference", "nms_threshold", 0.45f,
                   inference.nms_threshold, error) ||
        !get_float(data, "inference", "fusion_iou_threshold", 0.5f,
                   inference.fusion_iou_threshold, error) ||
        !get_float(data, "inference", "fusion_iom_threshold", 0.3f,
                   inference.fusion_iom_threshold, error) ||
        !get_float(data, "inference", "fusion_confidence_threshold", 0.3f,
                   inference.fusion_confidence_threshold, error)) {
        return false;
    }
    if (!get_int_list(data, "inference", "class_ids",
                      inference.class_ids, error)) {
        return false;
    }
    if (inference.core < 0 || inference.core > 2 ||
        inference.person_core < 0 || inference.person_core > 2 ||
        inference.helmet_core < 0 || inference.helmet_core > 2 ||
        inference.callplay_core < 0 || inference.callplay_core > 2) {
        error = "NPU core values must be 0, 1 or 2";
        return false;
    }
    if (inference.class_count < 1 ||
        inference.person_class_count < 1 || inference.helmet_class_count < 1 ||
        inference.callplay_class_count < 1) {
        error = "model class counts must be positive";
        return false;
    }
    for (size_t i = 0; i < inference.class_ids.size(); ++i) {
        const int class_id = inference.class_ids[i];
        if (class_id < 0 || class_id >= inference.class_count) {
            error = "[inference] class_ids must be within model class_count";
            return false;
        }
        if (std::find(inference.class_ids.begin(),
                      inference.class_ids.begin() + i,
                      class_id) != inference.class_ids.begin() + i) {
            error = "[inference] class_ids must not contain duplicates";
            return false;
        }
    }
    if (!in_unit_interval(inference.confidence_threshold) ||
        !in_unit_interval(inference.nms_threshold) ||
        !in_unit_interval(inference.fusion_iou_threshold) ||
        !in_unit_interval(inference.fusion_iom_threshold) ||
        !in_unit_interval(inference.fusion_confidence_threshold)) {
        error = "inference and fusion thresholds must be between 0 and 1";
        return false;
    }

    config.streams.clear();
    for (int id = 0; id < config.global.stream_count; ++id) {
        const std::string section = "stream." + std::to_string(id);
        if (data.find(section) == data.end()) {
            error = "missing [" + section + "] section";
            return false;
        }
        StreamSourceConfig source;
        source.id = id;
        if (!get_bool(data, section, "enabled", true, source.enabled, error) ||
            !get_int(data, section, "device_index", 0,
                     source.device_index, error) ||
            !get_int(data, section, "width", 0, source.width, error) ||
            !get_int(data, section, "height", 0, source.height, error) ||
            !get_int(data, section, "fps", 25, source.fps, error) ||
            !get_bool(data, section, "auto_white_balance", false,
                      source.auto_white_balance, error) ||
            !get_bool(data, section, "loop", true, source.loop, error) ||
            !get_int(data, section, "reconnect_interval_ms", 10000,
                     source.reconnect_interval_ms, error)) {
            return false;
        }
        const std::string type = get_string(data, section, "type");
        if (!parse_source_type(type, source.type)) {
            error = "[" + section + "] type must be mp4, rtsp or camera";
            return false;
        }
        source.url = get_string(data, section, "url");
        source.device_path = get_string(data, section, "device_path");
        source.chroma_order = lower(
            get_string(data, section, "chroma_order", "uv"));
        if (source.type == InputSourceType::Mp4) {
            source.url = resolve_path(base, source.url);
        }
        if (source.enabled && source.type != InputSourceType::Camera &&
            source.url.empty()) {
            error = "[" + section + "] url is required";
            return false;
        }
        if (source.fps < 1 || source.reconnect_interval_ms < 0 ||
            source.width < 0 || source.height < 0) {
            error = "invalid capture settings in [" + section + "]";
            return false;
        }
        if (source.type == InputSourceType::Camera &&
            source.chroma_order != "uv" && source.chroma_order != "vu") {
            error = "[" + section + "] chroma_order must be uv or vu";
            return false;
        }
        if (source.enabled) {
            config.streams.push_back(source);
        }
    }
    if (config.streams.empty()) {
        error = "at least one stream must be enabled";
        return false;
    }

    auto& streaming = config.streaming;
    streaming.rtmp_url = get_string(data, "streaming", "rtmp_url");
    streaming.rtsp_url = get_string(data, "streaming", "rtsp_url");
    if (!get_int(data, "streaming", "width", 1280,
                 streaming.width, error) ||
        !get_int(data, "streaming", "height", 720,
                 streaming.height, error) ||
        !get_int(data, "streaming", "fps", 24, streaming.fps, error) ||
        !get_int(data, "streaming", "bitrate", 2000000,
                 streaming.bitrate, error) ||
        !get_bool(data, "streaming", "enable_rtmp", true,
                  streaming.enable_rtmp, error) ||
        !get_bool(data, "streaming", "enable_rtsp", false,
                  streaming.enable_rtsp, error) ||
        !get_bool(data, "streaming", "draw_detections", true,
                  streaming.draw_detections, error)) {
        return false;
    }
    if (streaming.width < 16 || streaming.height < 16 || streaming.fps < 1 ||
        streaming.bitrate < 1) {
        error = "invalid dimensions, fps or bitrate in [streaming]";
        return false;
    }
    if (streaming.enable_rtmp && streaming.rtmp_url.empty()) {
        error = "[streaming] rtmp_url is required when RTMP is enabled";
        return false;
    }
    if (streaming.enable_rtsp && streaming.rtsp_url.empty()) {
        error = "[streaming] rtsp_url is required when RTSP is enabled";
        return false;
    }
    return true;
}
