#pragma once

#include <opencv2/core.hpp>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mias {

struct CameraConfig {
    std::string backend; // hikrobot, flir, or simulated
    std::string serial;
    std::string name;
    std::string pixel_format = "Mono8";
    int width = 0; // zero preserves the camera's current ROI
    int height = 0;
    double fps = 30.0;
    double exposure_us = 0; // zero preserves the current setting
    std::string trigger = "off"; // off or external
    std::string trigger_source = "Line0";
    std::string trigger_activation = "RisingEdge";
    std::uint64_t throughput_limit = 0; // bytes/second; zero preserves setting
};

struct DeviceInfo {
    std::string backend;
    std::string serial;
    std::string model;
};

struct Frame {
    cv::Mat image; // owns its pixels; only CV_8UC1 and CV_8UC3 are accepted
    std::uint64_t frame_id = 0;
    bool frame_id_valid = false;
    std::uint64_t camera_timestamp = 0;
    std::string camera_timestamp_unit = "unavailable";
    std::uint64_t host_monotonic_ns = 0;
    std::uint64_t host_unix_ns = 0;
};

// open() configures but does not begin acquisition. start()/read()/stop() are
// used by one acquisition thread per camera. A false read means timeout only;
// incomplete frames, disconnects and SDK failures must throw with context.
class Camera {
public:
    virtual ~Camera() = default;
    virtual void open(const CameraConfig&) = 0;
    virtual void start() = 0;
    virtual bool read(Frame&, unsigned timeout_ms) = 0;
    virtual void stop() noexcept = 0;
    virtual nlohmann::json info() const = 0;
};

std::vector<DeviceInfo> enumerate_hikrobot();
std::vector<DeviceInfo> enumerate_flir();
std::unique_ptr<Camera> make_hikrobot_camera();
std::unique_ptr<Camera> make_flir_camera();

inline std::uint64_t monotonic_ns();
inline std::uint64_t unix_ns();
} // namespace mias

#include <chrono>
namespace mias {
inline std::uint64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline std::uint64_t unix_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace mias
