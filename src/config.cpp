#include "mias/config.hpp"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace mias {
namespace {
using json = nlohmann::json;
void allowed_keys(const json& j, std::initializer_list<const char*> keys,
                  const std::string& context) {
    if (!j.is_object()) throw std::runtime_error(context + " must be an object");
    std::set<std::string> allowed(keys.begin(), keys.end());
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!allowed.count(it.key()))
            throw std::runtime_error(context + ": unknown setting '" + it.key() + "'");
}
std::uint64_t nonnegative_integer(const json& j, const char* key, std::uint64_t fallback) {
    if (!j.contains(key)) return fallback;
    if (!j.at(key).is_number_integer() ||
        (j.at(key).is_number_integer() && !j.at(key).is_number_unsigned() && j.at(key).get<std::int64_t>() < 0))
        throw std::runtime_error(std::string(key) + " must be a non-negative integer");
    return j.at(key).get<std::uint64_t>();
}
void finite_nonnegative(double value, const std::string& name, bool positive = false) {
    if (!std::isfinite(value) || value < 0 || (positive && value == 0))
        throw std::runtime_error(name + (positive ? " must be positive and finite" : " must be non-negative and finite"));
}
}

Config load_config(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open config: " + path);
    json j; input >> j;
    allowed_keys(j, {"schema_version", "output_dir", "record", "preview", "preview_fps",
        "codec", "container", "segment_seconds", "queue_frames", "queue_megabytes",
        "duration_seconds", "frame_limit", "no_frame_timeout_seconds", "cameras"}, "config");
    if (nonnegative_integer(j, "schema_version", 0) != 2)
        throw std::runtime_error("Expected schema_version 2; legacy Windows config is not accepted");
    Config c;
    c.output_dir = j.value("output_dir", c.output_dir);
    c.record = j.value("record", c.record);
    c.preview = j.value("preview", c.preview);
    c.preview_fps = j.value("preview_fps", c.preview_fps);
    c.codec = j.value("codec", c.codec);
    c.container = j.value("container", c.container);
    c.segment_seconds = j.value("segment_seconds", c.segment_seconds);
    c.queue_frames = nonnegative_integer(j, "queue_frames", c.queue_frames);
    c.queue_megabytes = nonnegative_integer(j, "queue_megabytes", c.queue_megabytes);
    c.duration_seconds = j.value("duration_seconds", c.duration_seconds);
    c.frame_limit = nonnegative_integer(j, "frame_limit", c.frame_limit);
    c.no_frame_timeout_seconds = j.value("no_frame_timeout_seconds", c.no_frame_timeout_seconds);
    if (!j.contains("cameras") || !j.at("cameras").is_array())
        throw std::runtime_error("cameras must be an array; see configs/ examples");
    for (const auto& a : j.at("cameras")) {
        allowed_keys(a, {"backend", "serial", "name", "pixel_format", "width", "height",
            "fps", "exposure_us", "trigger", "trigger_source", "trigger_activation", "throughput_limit"}, "camera");
        CameraConfig x;
        x.backend = a.at("backend").get<std::string>();
        x.serial = a.at("serial").get<std::string>();
        x.name = a.value("name", x.serial);
        x.pixel_format = a.value("pixel_format", x.pixel_format);
        const auto width = nonnegative_integer(a, "width", 0);
        const auto height = nonnegative_integer(a, "height", 0);
        if (width > 65536 || height > 65536) throw std::runtime_error("width/height out of range");
        x.width = static_cast<int>(width); x.height = static_cast<int>(height);
        x.fps = a.value("fps", x.fps); x.exposure_us = a.value("exposure_us", x.exposure_us);
        x.trigger = a.value("trigger", x.trigger);
        x.trigger_source = a.value("trigger_source", x.trigger_source);
        x.trigger_activation = a.value("trigger_activation", x.trigger_activation);
        x.throughput_limit = nonnegative_integer(a, "throughput_limit", 0);
        c.cameras.push_back(x);
    }
    validate_config(c);
    return c;
}

void validate_config(const Config& c) {
    if (c.output_dir.empty()) throw std::runtime_error("output_dir must not be empty");
    if (c.cameras.empty() || c.cameras.size() > 40)
        throw std::runtime_error("Configure between 1 and 40 cameras");
    if (c.queue_frames == 0 || c.queue_frames > 100000 || c.queue_megabytes == 0 || c.queue_megabytes > 65536)
        throw std::runtime_error("queue_frames must be 1..100000, queue_megabytes must be 1..65536");
    if (c.codec != "FFV1" && c.codec != "MJPG")
        throw std::runtime_error("codec must be FFV1 (lossless) or MJPG (lossy)");
    if (c.container != "avi" && !(c.container == "mkv" && c.codec == "FFV1"))
        throw std::runtime_error("Use avi, or mkv with FFV1");
    finite_nonnegative(c.segment_seconds, "segment_seconds", true);
    finite_nonnegative(c.preview_fps, "preview_fps", true);
    finite_nonnegative(c.duration_seconds, "duration_seconds");
    finite_nonnegative(c.no_frame_timeout_seconds, "no_frame_timeout_seconds", true);
    std::set<std::string> names, devices;
    const std::regex safe_name("[A-Za-z0-9][A-Za-z0-9_.-]{0,79}");
    const std::set<std::string> formats{"Mono8", "BayerGR8", "BayerRG8", "BayerGB8", "BayerBG8", "RGB8", "BGR8"};
    for (const auto& x : c.cameras) {
        if (x.backend != "hikrobot" && x.backend != "flir" && x.backend != "simulated")
            throw std::runtime_error("Unknown camera backend: " + x.backend);
        if (x.serial.empty() || x.serial.find("REPLACE") != std::string::npos)
            throw std::runtime_error("Set each real camera's serial from --list; placeholder serial found");
        if (!std::regex_match(x.name, safe_name))
            throw std::runtime_error("Camera name must be a safe 1..80 character filename: " + x.name);
        if (!names.insert(x.name).second) throw std::runtime_error("Duplicate output name: " + x.name);
        if (!devices.insert(x.backend + ":" + x.serial).second)
            throw std::runtime_error("Duplicate camera serial: " + x.serial);
        if (!formats.count(x.pixel_format)) throw std::runtime_error("Unsupported 8-bit pixel_format: " + x.pixel_format);
        if (x.width < 0 || x.height < 0 || x.width > 65536 || x.height > 65536)
            throw std::runtime_error("width/height out of range");
        // Many video backends truncate odd dimensions. Never allow silent cropping.
        if ((x.width && x.width % 2) || (x.height && x.height % 2))
            throw std::runtime_error("Video width and height must be even");
        finite_nonnegative(x.fps, "camera fps", true);
        finite_nonnegative(x.exposure_us, "exposure_us");
        if (x.fps > 100000) throw std::runtime_error("fps out of range");
        if (x.trigger != "off" && x.trigger != "external")
            throw std::runtime_error("trigger must be off or external");
        if (x.trigger_source.empty() || x.trigger_activation.empty())
            throw std::runtime_error("trigger_source/trigger_activation must not be empty");
        if (x.backend == "simulated" && x.trigger != "off")
            throw std::runtime_error("Simulator supports free-running capture only");
        if (x.pixel_format.rfind("Bayer", 0) == 0 && c.record && c.codec != "FFV1")
            throw std::runtime_error("Raw Bayer recording requires lossless FFV1");
    }
}

nlohmann::json config_json(const Config& c) {
    json cameras = json::array();
    for (const auto& x : c.cameras) cameras.push_back({{"backend", x.backend}, {"serial", x.serial},
        {"name", x.name}, {"pixel_format", x.pixel_format}, {"width", x.width}, {"height", x.height},
        {"fps", x.fps}, {"exposure_us", x.exposure_us}, {"trigger", x.trigger},
        {"trigger_source", x.trigger_source}, {"trigger_activation", x.trigger_activation},
        {"throughput_limit", x.throughput_limit}});
    return {{"schema_version", 2}, {"output_dir", c.output_dir}, {"record", c.record},
        {"preview", c.preview}, {"preview_fps", c.preview_fps}, {"codec", c.codec},
        {"container", c.container}, {"segment_seconds", c.segment_seconds},
        {"queue_frames", c.queue_frames}, {"queue_megabytes", c.queue_megabytes},
        {"duration_seconds", c.duration_seconds}, {"frame_limit", c.frame_limit},
        {"no_frame_timeout_seconds", c.no_frame_timeout_seconds}, {"cameras", cameras}};
}

Config simulated_config(int count) {
    if (count < 1 || count > 40) throw std::runtime_error("--simulate requires 1..40 cameras");
    Config c;
    for (int i = 1; i <= count; ++i) {
        std::ostringstream n; n << std::setw(2) << std::setfill('0') << i;
        CameraConfig x; x.backend = "simulated"; x.serial = "sim-" + n.str();
        x.name = "cam" + n.str(); x.width = 320; x.height = 240;
        c.cameras.push_back(x);
    }
    return c;
}
} // namespace mias
