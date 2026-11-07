#pragma once
#include "mias/config.hpp"
#include <opencv2/videoio.hpp>
#include <filesystem>
#include <fstream>

namespace mias {
class Recorder {
    Config config_;
    CameraConfig camera_;
    std::filesystem::path dir_;
    cv::VideoWriter video_;
    std::ofstream csv_;
    std::size_t segment_ = 0;
    std::uint64_t segment_frames_ = 0;
    std::uint64_t total_frames_ = 0;
    std::uint64_t segment_start_ns_ = 0;
    std::string filename_;
    cv::Size size_;
    int type_ = -1;
    nlohmann::json segments_ = nlohmann::json::array();
    void finish_segment();
    void begin_segment(const Frame& frame);
public:
    Recorder(const Config&, const CameraConfig&, const std::filesystem::path& directory);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    void write(const Frame&, std::uint64_t capture_index);
    void close();
    std::uint64_t submitted() const { return total_frames_; }
    const nlohmann::json& segments() const { return segments_; }
};
void save_json_atomic(const std::filesystem::path&, const nlohmann::json&);
} // namespace mias
