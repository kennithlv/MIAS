#include "mias/camera.hpp"

#include <stdexcept>

#if defined(MIAS_WITH_FLIR) && MIAS_WITH_FLIR
#include <Spinnaker.h>
#include <SpinGenApi/SpinnakerGenApi.h>
#include <opencv2/imgproc.hpp>
#include <limits>

namespace mias {
namespace {
using namespace Spinnaker;
using namespace Spinnaker::GenApi;

// All cameras share one System lifetime. Releasing one camera must not release
// the singleton out from under another acquisition thread.
struct SpinRuntime {
    SystemPtr system = System::GetInstance();
    ~SpinRuntime() {
        try { system->ReleaseInstance(); } catch (...) {}
    }
};

std::shared_ptr<SpinRuntime> acquire_spin() {
    static const auto shared = std::make_shared<SpinRuntime>();
    return shared;
}

std::string read_string(INodeMap& map, const char* name) {
    CStringPtr node = map.GetNode(name);
    if (!IsReadable(node)) return "";
    return node->GetValue().c_str();
}

void set_enum(INodeMap& map, const char* name, const std::string& value, bool optional = false) {
    CEnumerationPtr node = map.GetNode(name);
    if (!IsAvailable(node)) {
        if (optional) return;
        throw std::runtime_error(std::string("FLIR node is unavailable: ") + name);
    }
    if (!IsWritable(node)) throw std::runtime_error(std::string("FLIR node is not writable: ") + name);
    CEnumEntryPtr entry = node->GetEntryByName(value.c_str());
    if (!IsReadable(entry)) throw std::runtime_error(std::string("FLIR ") + name + " does not support '" + value + "'");
    node->SetIntValue(entry->GetValue());
}

void set_integer(INodeMap& map, const char* name, std::int64_t value, bool optional = false) {
    CIntegerPtr node = map.GetNode(name);
    if (!IsAvailable(node)) {
        if (optional) return;
        throw std::runtime_error(std::string("FLIR integer node is unavailable: ") + name);
    }
    if (!IsWritable(node)) throw std::runtime_error(std::string("FLIR integer node is not writable: ") + name);
    const auto step = node->GetInc();
    if (value < node->GetMin() || value > node->GetMax() || (step > 0 && (value - node->GetMin()) % step != 0))
        throw std::runtime_error(std::string("FLIR setting outside node range/increment: ") + name + "=" + std::to_string(value));
    node->SetValue(value);
}

void set_float(INodeMap& map, const char* name, double value, const char* alternate = nullptr) {
    CFloatPtr node = map.GetNode(name);
    if (!IsAvailable(node) && alternate) node = map.GetNode(alternate);
    if (!IsAvailable(node) || !IsWritable(node))
        throw std::runtime_error(std::string("FLIR float node is unavailable or not writable: ") + name);
    if (value < node->GetMin() || value > node->GetMax())
        throw std::runtime_error(std::string("FLIR setting outside node range: ") + name + "=" + std::to_string(value) +
                                 "; check exposure, ROI, pixel format and link bandwidth");
    node->SetValue(value);
}

void set_fps_enable(INodeMap& map, bool enabled) {
    CBooleanPtr node = map.GetNode("AcquisitionFrameRateEnable");
    if (!IsAvailable(node)) node = map.GetNode("AcquisitionFrameRateEnabled"); // Chameleon3/Gen2
    if (IsAvailable(node)) {
        if (!IsWritable(node)) throw std::runtime_error("FLIR frame-rate enable node is not writable");
        node->SetValue(enabled);
    }
}

int channels_for(const std::string& format) {
    if (format == "Mono8" || format == "BayerGR8" || format == "BayerRG8" ||
        format == "BayerGB8" || format == "BayerBG8") return 1;
    if (format == "RGB8" || format == "BGR8") return 3;
    throw std::runtime_error("FLIR unsupported pixel_format '" + format +
        "': use Mono8, BayerGR8/RG8/GB8/BG8, BGR8 or RGB8");
}

class FlirCamera final : public Camera {
public:
    ~FlirCamera() override { stop(); }

    void open(const CameraConfig& config) override {
        stop();
        config_ = config;
        channels_for(config.pixel_format);
        if (config.serial.empty()) throw std::runtime_error("FLIR requires an exact camera serial number");
        if (config.trigger != "off" && config.trigger != "external")
            throw std::runtime_error("FLIR trigger must be 'off' or 'external'");
        try {
            runtime_ = acquire_spin();
            auto list = runtime_->system->GetCameras();
            for (unsigned i = 0; i < list.GetSize(); ++i) {
                auto candidate = list.GetByIndex(i);
                if (read_string(candidate->GetTLDeviceNodeMap(), "DeviceSerialNumber") == config.serial) {
                    camera_ = candidate;
                    break;
                }
            }
            list.Clear();
            if (!camera_) throw std::runtime_error("FLIR camera serial '" + config.serial + "' was not found");
            camera_->Init();
            initialized_ = true;
            auto& map = camera_->GetNodeMap();
            set_enum(map, "TriggerSelector", "FrameStart", true);
            set_enum(map, "TriggerMode", "Off");
            set_enum(map, "AcquisitionMode", "Continuous");
            set_enum(map, "PixelFormat", config.pixel_format);
            if (config.width > 0) {
                set_integer(map, "OffsetX", 0, true);
                set_integer(map, "Width", config.width);
            }
            if (config.height > 0) {
                set_integer(map, "OffsetY", 0, true);
                set_integer(map, "Height", config.height);
            }
            if (config.exposure_us > 0) {
                set_enum(map, "ExposureAuto", "Off", true);
                set_enum(map, "ExposureMode", "Timed", true);
                set_float(map, "ExposureTime", config.exposure_us, "ExposureTimeAbs");
            }
            if (config.throughput_limit > 0) {
                if (config.throughput_limit > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                    throw std::runtime_error("FLIR throughput_limit is out of range");
                set_enum(map, "DeviceLinkThroughputLimitMode", "On", true);
                set_integer(map, "DeviceLinkThroughputLimit", static_cast<std::int64_t>(config.throughput_limit));
            }
            if (config.trigger == "external") {
                set_fps_enable(map, false);
                set_enum(map, "TriggerSource", config.trigger_source);
                set_enum(map, "TriggerActivation", config.trigger_activation);
                set_enum(map, "TriggerMode", "On");
            } else {
                set_fps_enable(map, true);
                set_enum(map, "AcquisitionFrameRateAuto", "Off", true);
                set_float(map, "AcquisitionFrameRate", config.fps, "AcquisitionFrameRateAbs");
            }
            // NewestOnly intentionally drops older frames; acquisition for
            // recording requires oldest-first delivery instead.
            auto& stream = camera_->GetTLStreamNodeMap();
            set_enum(stream, "StreamBufferHandlingMode", "OldestFirst", true);
            CIntegerPtr width = map.GetNode("Width");
            CIntegerPtr height = map.GetNode("Height");
            if (!IsReadable(width) || !IsReadable(height)) throw std::runtime_error("FLIR cannot read image dimensions");
            details_ = {{"backend", "flir"}, {"serial", config.serial},
                        {"model", read_string(camera_->GetTLDeviceNodeMap(), "DeviceModelName")},
                        {"width", width->GetValue()}, {"height", height->GetValue()},
                        {"pixel_format", config.pixel_format}, {"stored_channels", channels_for(config.pixel_format)},
                        {"requested_fps", config.fps}, {"trigger", config.trigger},
                        {"camera_timestamp_unit", "ns"}, {"frame_id_bits", 64},
                        {"raw_bayer", config.pixel_format.rfind("Bayer", 0) == 0}};
            CFloatPtr resulting = map.GetNode("AcquisitionResultingFrameRate");
            if (!IsReadable(resulting)) resulting = map.GetNode("ResultingFrameRate");
            if (IsReadable(resulting)) details_["resulting_fps"] = resulting->GetValue();
            if (config.trigger == "external") {
                details_["trigger_source"] = config.trigger_source;
                details_["trigger_activation"] = config.trigger_activation;
                details_["fps_note"] = "Configured fps is the video playback rate; external pulses determine acquisition timing.";
            }
        } catch (const Spinnaker::Exception& error) {
            stop();
            throw std::runtime_error("FLIR open [" + config.serial + "]: " + error.what());
        } catch (...) {
            stop();
            throw;
        }
    }

    void start() override {
        if (!initialized_) throw std::runtime_error("FLIR camera must be opened before start");
        try {
            camera_->BeginAcquisition();
            started_ = true;
        } catch (const Spinnaker::Exception& error) {
            throw std::runtime_error("FLIR start [" + config_.serial + "]: " + error.what());
        }
    }

    bool read(Frame& output, unsigned timeout_ms) override {
        if (!started_) throw std::runtime_error("FLIR camera is not acquiring");
        try {
            ImagePtr image = camera_->GetNextImage(timeout_ms);
            struct ImageGuard {
                ImagePtr image;
                ~ImageGuard() { try { image->Release(); } catch (...) {} }
            } guard{image};
            const auto host_mono = monotonic_ns();
            const auto host_wall = unix_ns();
            if (image->IsIncomplete())
                throw std::runtime_error("FLIR " + config_.serial + " returned an incomplete image (status " +
                                         std::to_string(static_cast<int>(image->GetImageStatus())) + ")");
            const std::string format = image->GetPixelFormatName().c_str();
            if (format != config_.pixel_format)
                throw std::runtime_error("FLIR " + config_.serial + " pixel format changed to " + format);
            const auto channels = channels_for(format);
            const auto width = image->GetWidth();
            const auto height = image->GetHeight();
            const auto stride = image->GetStride();
            const auto row_bytes = width * channels;
            if (width == 0 || height == 0 || width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
                height > static_cast<std::size_t>(std::numeric_limits<int>::max()) || !image->GetData() ||
                stride < row_bytes || (height - 1) > (std::numeric_limits<std::size_t>::max() - row_bytes) / stride ||
                image->GetBufferSize() < (height - 1) * stride + row_bytes)
                throw std::runtime_error("FLIR " + config_.serial + " returned invalid image dimensions/stride/payload");
            cv::Mat borrowed(static_cast<int>(height), static_cast<int>(width),
                             channels == 3 ? CV_8UC3 : CV_8UC1, image->GetData(), stride);
            if (format == "RGB8") cv::cvtColor(borrowed, output.image, cv::COLOR_RGB2BGR);
            else output.image = borrowed.clone();
            output.frame_id = image->GetFrameID();
            output.frame_id_valid = true;
            output.camera_timestamp = image->GetTimeStamp();
            output.camera_timestamp_unit = "ns";
            output.host_monotonic_ns = host_mono;
            output.host_unix_ns = host_wall;
            return true;
        } catch (const Spinnaker::Exception& error) {
            if (error.GetError() == SPINNAKER_ERR_TIMEOUT) return false;
            throw std::runtime_error("FLIR read [" + config_.serial + "]: " + error.what());
        }
    }

    void stop() noexcept override {
        if (camera_) {
            if (started_) { try { camera_->EndAcquisition(); } catch (...) {} }
            if (initialized_) { try { camera_->DeInit(); } catch (...) {} }
            camera_ = nullptr;
        }
        started_ = false;
        initialized_ = false;
        runtime_.reset();
    }

    nlohmann::json info() const override { return details_; }

private:
    CameraConfig config_;
    std::shared_ptr<SpinRuntime> runtime_;
    CameraPtr camera_;
    bool initialized_ = false;
    bool started_ = false;
    nlohmann::json details_;
};
} // namespace

std::vector<DeviceInfo> enumerate_flir() {
    auto runtime = acquire_spin();
    std::vector<DeviceInfo> result;
    auto list = runtime->system->GetCameras();
    for (unsigned i = 0; i < list.GetSize(); ++i) {
        auto camera = list.GetByIndex(i);
        auto& map = camera->GetTLDeviceNodeMap();
        result.push_back({"flir", read_string(map, "DeviceSerialNumber"), read_string(map, "DeviceModelName")});
    }
    list.Clear();
    return result;
}

std::unique_ptr<Camera> make_flir_camera() { return std::make_unique<FlirCamera>(); }
} // namespace mias

#else
namespace mias {
std::vector<DeviceInfo> enumerate_flir() { return {}; }
std::unique_ptr<Camera> make_flir_camera() {
    throw std::runtime_error("FLIR backend is disabled: install the Linux Spinnaker SDK and rebuild with -DMIAS_WITH_FLIR=ON");
}
} // namespace mias
#endif
