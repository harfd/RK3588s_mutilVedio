#pragma once

#include <string>
#include <vector>

enum class InputSourceType {
    Mp4,
    Rtsp,
    Camera,
};

struct StreamSourceConfig {
    int id = 0;
    bool enabled = true;
    InputSourceType type = InputSourceType::Mp4;
    std::string url;
    int device_index = 0;
    std::string device_path;
    int width = 0;
    int height = 0;
    int fps = 25;
    std::string chroma_order = "uv";
    bool auto_white_balance = false;
    bool loop = true;
    int reconnect_interval_ms = 10000;
};

struct GlobalAppConfig {
    int stream_count = 1;
    int infer_interval = 2;
};

struct InferenceAppConfig {
    std::string person_model_path;
    std::string helmet_model_path;
    std::string callplay_model_path;
    int person_core = 0;
    int helmet_core = 1;
    int callplay_core = 2;
    int person_class_count = 1;
    int helmet_class_count = 2;
    int callplay_class_count = 2;
    float confidence_threshold = 0.25f;
    float nms_threshold = 0.45f;
    float fusion_iou_threshold = 0.5f;
    float fusion_iom_threshold = 0.3f;
    float fusion_confidence_threshold = 0.3f;
};

struct OutputStreamingConfig {
    std::string rtmp_url;
    std::string rtsp_url;
    int width = 1280;
    int height = 720;
    int fps = 24;
    int bitrate = 2000000;
    bool enable_rtmp = true;
    bool enable_rtsp = false;
    bool draw_detections = true;
};

struct AppConfig {
    GlobalAppConfig global;
    InferenceAppConfig inference;
    OutputStreamingConfig streaming;
    std::vector<StreamSourceConfig> streams;
};

class AppConfigLoader {
public:
    static bool load(const std::string& file_path, AppConfig& config,
                     std::string& error);
};

const char* input_source_type_name(InputSourceType type);
