#pragma once
#include "mias/camera.hpp"
#include <cstddef>

namespace mias {
struct Config {
    std::string output_dir = "video";
    bool record = true;
    bool preview = false;
    double preview_fps = 10;
    std::string codec = "FFV1";
    std::string container = "avi";
    double segment_seconds = 30;
    std::size_t queue_frames = 64;
    std::size_t queue_megabytes = 256;
    double duration_seconds = 0;
    std::uint64_t frame_limit = 0;
    double no_frame_timeout_seconds = 10;
    std::vector<CameraConfig> cameras;
};
Config load_config(const std::string& path);
void validate_config(const Config& config);
nlohmann::json config_json(const Config& config);
Config simulated_config(int count);
std::unique_ptr<Camera> make_simulated_camera();
} // namespace mias
