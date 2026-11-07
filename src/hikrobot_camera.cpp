#include "mias/camera.hpp"

#include <stdexcept>

#if defined(MIAS_WITH_HIKROBOT) && MIAS_WITH_HIKROBOT
#include <MvCameraControl.h>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace mias {
namespace {

void check_mv(int code, const std::string& operation) {
    if (code == MV_OK) return;
    std::ostringstream message;
    message << "Hikrobot " << operation << " failed (SDK error 0x"
            << std::hex << std::uppercase << static_cast<unsigned>(code) << ")";
    throw std::runtime_error(message.str());
}

// Current MVS SDKs require one process-level initialization. Keep this owner
// alive until process exit, including across enumeration followed by open().
struct MvsRuntime {
    MvsRuntime() { check_mv(MV_CC_Initialize(), "initialization"); }
    ~MvsRuntime() { MV_CC_Finalize(); }
};

std::shared_ptr<MvsRuntime> acquire_mvs() {
    static const auto shared = std::make_shared<MvsRuntime>();
    return shared;
}

template <typename T, std::size_t N>
std::string sdk_string(const T (&value)[N]) {
    const char* begin = reinterpret_cast<const char*>(value);
    const char* end = std::find(begin, begin + N, '\0');
    return std::string(begin, end);
}

int channels_for(const std::string& pixel_format) {
    if (pixel_format == "Mono8" || pixel_format == "BayerGR8" ||
        pixel_format == "BayerRG8" || pixel_format == "BayerGB8" ||
        pixel_format == "BayerBG8") return 1;
    if (pixel_format == "BGR8" || pixel_format == "RGB8") return 3;
    throw std::runtime_error("Hikrobot unsupported pixel_format '" + pixel_format +
        "': use Mono8, BayerGR8/RG8/GB8/BG8, BGR8 or RGB8");
}

class HikrobotCamera final : public Camera {
public:
    ~HikrobotCamera() override { stop(); }

    void open(const CameraConfig& config) override {
        stop();
        config_ = config;
        channels_ = channels_for(config.pixel_format);
        if (config.serial.empty()) throw std::runtime_error("Hikrobot requires an exact camera serial number");
        if (config.trigger != "off" && config.trigger != "external")
            throw std::runtime_error("Hikrobot trigger must be 'off' or 'external'");
        try {
            runtime_ = acquire_mvs();
            MV_CC_DEVICE_INFO_LIST devices{};
            check_mv(MV_CC_EnumDevices(MV_USB_DEVICE, &devices), "USB enumeration");
            MV_CC_DEVICE_INFO* found = nullptr;
            for (unsigned i = 0; i < devices.nDeviceNum; ++i) {
                auto* candidate = devices.pDeviceInfo[i];
                if (candidate && candidate->nTLayerType == MV_USB_DEVICE &&
                    sdk_string(candidate->SpecialInfo.stUsb3VInfo.chSerialNumber) == config.serial) {
                    found = candidate;
                    break;
                }
            }
            if (!found) throw std::runtime_error("Hikrobot USB camera serial '" + config.serial + "' was not found");
            const auto model = sdk_string(found->SpecialInfo.stUsb3VInfo.chModelName);
            check_mv(MV_CC_CreateHandle(&handle_, found), "create handle for " + config.serial);
            check_mv(MV_CC_OpenDevice(handle_), "open " + config.serial);
            opened_ = true;

            // Configure before acquisition. Never silently fall back to a
            // different requested pixel format, ROI, trigger source or rate.
            set_enum_if_present("TriggerSelector", "FrameStart");
            set_enum("TriggerMode", "Off");
            set_enum("AcquisitionMode", "Continuous");
            set_enum("PixelFormat", config.pixel_format);
            if (config.width > 0) {
                set_int_if_present("OffsetX", 0);
                set_int("Width", config.width);
            }
            if (config.height > 0) {
                set_int_if_present("OffsetY", 0);
                set_int("Height", config.height);
            }
            // GetImageBuffer's legacy frame descriptor has no row stride. We
            // accept only a verified tightly packed image payload below.
            set_bool_if_present("ChunkModeActive", false);
            if (config.exposure_us > 0) {
                set_enum_if_present("ExposureAuto", "Off");
                set_enum_if_present("ExposureMode", "Timed");
                set_float("ExposureTime", config.exposure_us);
            }
            if (config.throughput_limit > 0) {
                set_enum_if_present("DeviceLinkThroughputLimitMode", "On");
                set_int("DeviceLinkThroughputLimit", config.throughput_limit);
            }
            if (config.trigger == "external") {
                set_bool_if_present("AcquisitionFrameRateEnable", false);
                set_enum("TriggerSource", config.trigger_source);
                set_enum("TriggerActivation", config.trigger_activation);
                set_enum("TriggerMode", "On");
            } else {
                set_bool_if_present("AcquisitionFrameRateEnable", true);
                set_float("AcquisitionFrameRate", config.fps);
            }
            MVCC_ENUMVALUE pixel{};
            check_mv(MV_CC_GetEnumValue(handle_, "PixelFormat", &pixel), label("read PixelFormat"));
            pixel_type_ = pixel.nCurValue;
            const auto width = get_int("Width");
            const auto height = get_int("Height");
            details_ = {{"backend", "hikrobot"}, {"serial", config.serial}, {"model", model},
                        {"width", width}, {"height", height}, {"pixel_format", config.pixel_format},
                        {"stored_channels", channels_}, {"requested_fps", config.fps},
                        {"trigger", config.trigger}, {"camera_timestamp_unit", "device_ticks"},
                        {"frame_id_bits", 32}, {"raw_bayer", config.pixel_format.rfind("Bayer", 0) == 0}};
            MVCC_FLOATVALUE resulting{};
            if (MV_CC_GetFloatValue(handle_, "ResultingFrameRate", &resulting) == MV_OK)
                details_["resulting_fps"] = resulting.fCurValue;
            if (config.trigger == "external") {
                details_["trigger_source"] = config.trigger_source;
                details_["trigger_activation"] = config.trigger_activation;
                details_["fps_note"] = "Configured fps is the video playback rate; external pulses determine acquisition timing.";
            }
        } catch (...) {
            stop();
            throw;
        }
    }

    void start() override {
        if (!opened_) throw std::runtime_error("Hikrobot camera must be opened before start");
        check_mv(MV_CC_StartGrabbing(handle_), label("start grabbing"));
        started_ = true;
    }

    bool read(Frame& output, unsigned timeout_ms) override {
        if (!started_) throw std::runtime_error("Hikrobot camera is not acquiring");
        MV_FRAME_OUT frame{};
        const int result = MV_CC_GetImageBuffer(handle_, &frame, timeout_ms);
        if (static_cast<unsigned>(result) == static_cast<unsigned>(MV_E_NODATA)) {
            if (!MV_CC_IsDeviceConnected(handle_))
                throw std::runtime_error("Hikrobot camera " + config_.serial + " disconnected");
            return false;
        }
        check_mv(result, label("get image"));
        struct BufferGuard {
            void* handle;
            MV_FRAME_OUT* frame;
            ~BufferGuard() { MV_CC_FreeImageBuffer(handle, frame); }
        } guard{handle_, &frame};
        const auto host_mono = monotonic_ns();
        const auto host_wall = unix_ns();
        const auto& meta = frame.stFrameInfo;
        if (!frame.pBufAddr || meta.nWidth == 0 || meta.nHeight == 0)
            throw std::runtime_error("Hikrobot " + config_.serial + " returned an empty image");
        if (meta.nLostPacket != 0)
            throw std::runtime_error("Hikrobot " + config_.serial + " returned a frame with missing packets");
        if (static_cast<unsigned>(meta.enPixelType) != pixel_type_)
            throw std::runtime_error("Hikrobot " + config_.serial + " pixel format changed during acquisition");
        const std::size_t row_bytes = std::size_t(meta.nWidth) * channels_;
        const std::size_t required_bytes = row_bytes * meta.nHeight;
        if (meta.nFrameLen != required_bytes)
            throw std::runtime_error("Hikrobot " + config_.serial +
                " image payload is not tightly packed 8-bit data; disable chunks/padding or use a supported format");
        const int type = channels_ == 3 ? CV_8UC3 : CV_8UC1;
        cv::Mat borrowed(static_cast<int>(meta.nHeight), static_cast<int>(meta.nWidth),
                         type, frame.pBufAddr, row_bytes);
        if (config_.pixel_format == "RGB8") cv::cvtColor(borrowed, output.image, cv::COLOR_RGB2BGR);
        else output.image = borrowed.clone();
        output.frame_id = meta.nFrameNum;
        output.frame_id_valid = true;
        output.camera_timestamp = (std::uint64_t(meta.nDevTimeStampHigh) << 32) | meta.nDevTimeStampLow;
        output.camera_timestamp_unit = "device_ticks";
        output.host_monotonic_ns = host_mono;
        output.host_unix_ns = host_wall;
        return true;
    }

    void stop() noexcept override {
        if (handle_) {
            if (started_) MV_CC_StopGrabbing(handle_);
            if (opened_) MV_CC_CloseDevice(handle_);
            MV_CC_DestroyHandle(handle_);
        }
        handle_ = nullptr;
        opened_ = false;
        started_ = false;
        runtime_.reset();
    }

    nlohmann::json info() const override { return details_; }

private:
    std::string label(const std::string& operation) const { return operation + " [" + config_.serial + "]"; }
    void set_enum(const char* node, const std::string& value) {
        check_mv(MV_CC_SetEnumValueByString(handle_, node, value.c_str()), label(std::string(node) + "=" + value));
    }
    void set_enum_if_present(const char* node, const std::string& value) {
        MVCC_ENUMVALUE current{};
        if (MV_CC_GetEnumValue(handle_, node, &current) == MV_OK) set_enum(node, value);
    }
    void set_bool_if_present(const char* node, bool value) {
        bool current = false;
        if (MV_CC_GetBoolValue(handle_, node, &current) == MV_OK)
            check_mv(MV_CC_SetBoolValue(handle_, node, value), label(node));
    }
    void set_int(const char* node, std::uint64_t value) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::runtime_error("Hikrobot integer setting is out of range");
        check_mv(MV_CC_SetIntValueEx(handle_, node, static_cast<std::int64_t>(value)), label(node));
    }
    void set_int_if_present(const char* node, std::uint64_t value) {
        MVCC_INTVALUE_EX current{};
        if (MV_CC_GetIntValueEx(handle_, node, &current) == MV_OK) set_int(node, value);
    }
    std::int64_t get_int(const char* node) const {
        MVCC_INTVALUE_EX current{};
        check_mv(MV_CC_GetIntValueEx(handle_, node, &current), label(std::string("read ") + node));
        return current.nCurValue;
    }
    void set_float(const char* node, double value) {
        check_mv(MV_CC_SetFloatValue(handle_, node, static_cast<float>(value)), label(node));
    }

    CameraConfig config_;
    std::shared_ptr<MvsRuntime> runtime_;
    void* handle_ = nullptr;
    bool opened_ = false;
    bool started_ = false;
    int channels_ = 1;
    unsigned pixel_type_ = 0;
    nlohmann::json details_;
};
} // namespace

std::vector<DeviceInfo> enumerate_hikrobot() {
    auto runtime = acquire_mvs();
    MV_CC_DEVICE_INFO_LIST devices{};
    check_mv(MV_CC_EnumDevices(MV_USB_DEVICE, &devices), "USB enumeration");
    std::vector<DeviceInfo> result;
    for (unsigned i = 0; i < devices.nDeviceNum; ++i) {
        auto* camera = devices.pDeviceInfo[i];
        if (camera && camera->nTLayerType == MV_USB_DEVICE)
            result.push_back({"hikrobot", sdk_string(camera->SpecialInfo.stUsb3VInfo.chSerialNumber),
                              sdk_string(camera->SpecialInfo.stUsb3VInfo.chModelName)});
    }
    return result;
}

std::unique_ptr<Camera> make_hikrobot_camera() { return std::make_unique<HikrobotCamera>(); }
} // namespace mias

#else
namespace mias {
std::vector<DeviceInfo> enumerate_hikrobot() { return {}; }
std::unique_ptr<Camera> make_hikrobot_camera() {
    throw std::runtime_error("Hikrobot backend is disabled: install the Linux MVS SDK and rebuild with -DMIAS_WITH_HIKROBOT=ON");
}
} // namespace mias
#endif
