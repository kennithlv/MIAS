#include "mias/config.hpp"
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <thread>
#include <stdexcept>

namespace mias {
namespace {
class SimulatedCamera final : public Camera {
    CameraConfig config_;
    bool running_ = false;
    std::uint64_t frame_id_ = 0;
    std::chrono::steady_clock::time_point next_;
public:
    void open(const CameraConfig& c) override {
        config_ = c;
        if (!config_.width) config_.width = 320;
        if (!config_.height) config_.height = 240;
    }
    void start() override { running_ = true; next_ = std::chrono::steady_clock::now(); }
    bool read(Frame& frame, unsigned timeout_ms) override {
        if (!running_) throw std::runtime_error("Simulator not started");
        const auto now = std::chrono::steady_clock::now();
        if (next_ > now + std::chrono::milliseconds(timeout_ms)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms)); return false;
        }
        std::this_thread::sleep_until(next_);
        next_ += std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 / config_.fps));
        frame = Frame{};
        const bool color = config_.pixel_format == "RGB8" || config_.pixel_format == "BGR8";
        frame.image = cv::Mat(config_.height, config_.width, color ? CV_8UC3 : CV_8UC1, cv::Scalar::all(16));
        const int x = static_cast<int>((frame_id_ * 7) % config_.width);
        cv::rectangle(frame.image, cv::Rect(x, 0, std::min(12, config_.width - x), config_.height), cv::Scalar::all(220), cv::FILLED);
        cv::putText(frame.image, config_.serial + " frame " + std::to_string(frame_id_ + 1),
            cv::Point(8, std::min(35, config_.height - 1)), cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar::all(255));
        frame.frame_id = ++frame_id_; frame.frame_id_valid = true;
        frame.host_monotonic_ns = monotonic_ns(); frame.host_unix_ns = unix_ns();
        frame.camera_timestamp = frame.host_monotonic_ns; frame.camera_timestamp_unit = "ns_simulated";
        return true;
    }
    void stop() noexcept override { running_ = false; }
    nlohmann::json info() const override {
        return {{"model", "SIMULATED - no physical camera"}, {"serial", config_.serial},
            {"width", config_.width}, {"height", config_.height}, {"pixel_format", config_.pixel_format},
            {"requested_fps", config_.fps}, {"hardware_validated", false}};
    }
};
}
std::unique_ptr<Camera> make_simulated_camera() { return std::make_unique<SimulatedCamera>(); }
} // namespace mias
