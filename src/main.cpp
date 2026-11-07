#include "mias/config.hpp"
#include "mias/queue.hpp"
#include "mias/recorder.hpp"
#ifdef MIAS_WITH_PREVIEW
#include <opencv2/highgui.hpp>
#endif
#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <cmath>

namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace mias;
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }

void help() {
    std::cout << "MIAS Linux multi-camera video recorder\n"
        "  MIAS --config config.json [--duration SEC] [--frames N] [--output DIR]\n"
        "  MIAS --list [--backend hikrobot|flir]\n"
        "  MIAS --simulate 5 --frames 60 --output test-video\n"
        "Options: --no-record (receive only), --preview, --help\n"
        "Ctrl+C / SIGTERM stops acquisition, drains queued frames and closes videos.\n";
}
std::uint64_t integer_arg(const std::string& s) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Expected non-negative integer: " + s);
    return std::stoull(s);
}
double real_arg(const std::string& s) {
    std::size_t end = 0; const double value = std::stod(s, &end);
    if (end != s.size() || !std::isfinite(value) || value < 0)
        throw std::runtime_error("Expected non-negative finite number: " + s);
    return value;
}
std::unique_ptr<Camera> make_camera(const std::string& backend) {
    if (backend == "simulated") return make_simulated_camera();
    if (backend == "hikrobot") return make_hikrobot_camera();
    if (backend == "flir") return make_flir_camera();
    throw std::runtime_error("Unknown backend: " + backend);
}
fs::path session_directory(const std::string& root) {
    fs::create_directories(root);
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    std::ostringstream name;
    name << "session-" << std::put_time(std::gmtime(&t), "%Y%m%dT%H%M%S") << '-' << micros % 1000000;
    auto path = fs::path(root) / name.str();
    if (!fs::create_directory(path)) throw std::runtime_error("Session directory collision: " + path.string());
    return path;
}
struct CameraRun {
    CameraConfig config;
    std::unique_ptr<Camera> camera;
    FrameQueue queue;
    std::unique_ptr<Recorder> recorder;
    std::thread reader, writer;
    std::atomic<std::uint64_t> captured{0}, submitted{0}, gaps{0}, discontinuities{0}, timeouts{0};
    std::atomic<bool> done{false};
    std::mutex mutex;
    cv::Mat latest;
    std::string error;
    json device_info;
    CameraRun(const Config& c, const CameraConfig& cc)
        : config(cc), camera(make_camera(cc.backend)), queue(c.queue_frames, c.queue_megabytes * 1024ULL * 1024ULL) {}
};
json camera_summary(CameraRun& r) {
    std::lock_guard<std::mutex> lock(r.mutex);
    json j{{"name", r.config.name}, {"backend", r.config.backend}, {"serial", r.config.serial},
        {"device_info", r.device_info}, {"captured", r.captured.load()},
        {"frames_submitted", r.submitted.load()}, {"frame_id_gaps", r.gaps.load()},
        {"frame_id_discontinuities", r.discontinuities.load()},
        {"timeouts", r.timeouts.load()}, {"queue_high_water_frames", r.queue.high_water()}, {"error", r.error}};
    if (r.recorder) j["segments"] = r.recorder->segments();
    return j;
}

int run(const Config& config) {
    validate_config(config);
#ifndef MIAS_WITH_PREVIEW
    if (config.preview) throw std::runtime_error("Preview was not compiled; rebuild with -DMIAS_WITH_PREVIEW=ON");
#endif
    const fs::path session = session_directory(config.output_dir);
    std::cout << "Session: " << fs::absolute(session) << '\n';
    json manifest{{"schema_version", 2}, {"status", "starting"}, {"config", config_json(config)},
        {"started_unix_ns", unix_ns()}, {"camera_timestamps_synchronized", false},
        {"video_frame_count_semantics", "frames_submitted counts encoder calls; decode files to verify encoded frames"},
        {"cameras", json::array()}};
    save_json_atomic(session / "manifest.json", manifest);
    std::atomic<bool> stop{false};
    std::vector<std::unique_ptr<CameraRun>> runs;
    std::mutex errors_mutex;
    std::string session_error;
    auto fail = [&](CameraRun* r, const std::string& message) {
        if (r) { std::lock_guard<std::mutex> lock(r->mutex); if (r->error.empty()) r->error = message; }
        { std::lock_guard<std::mutex> lock(errors_mutex); if (session_error.empty()) session_error = message;
          std::cerr << "ERROR: " << message << '\n'; }
        stop = true;
    };
    const auto session_start = std::chrono::steady_clock::now();
    auto capture_start = session_start;
    try {
        // Fail missing/unsupported devices before starting any acquisition.
        for (const auto& cc : config.cameras) {
            auto r = std::make_unique<CameraRun>(config, cc);
            r->camera->open(cc); r->device_info = r->camera->info();
            if (config.record) r->recorder = std::make_unique<Recorder>(config, cc, session / cc.name);
            runs.push_back(std::move(r));
        }
        // SDK enumeration/configuration is setup time, not requested capture time.
        capture_start = std::chrono::steady_clock::now();
        manifest["capture_started_unix_ns"] = unix_ns();
        manifest["setup_seconds"] = std::chrono::duration<double>(capture_start - session_start).count();
        manifest["status"] = "recording";
        for (const auto& r : runs) manifest["cameras"].push_back(camera_summary(*r));
        save_json_atomic(session / "manifest.json", manifest);
        for (auto& owned : runs) {
            auto* r = owned.get();
            if (config.record) r->writer = std::thread([&, r] {
                try {
                    QueuedFrame item;
                    while (r->queue.pop(item)) {
                        r->recorder->write(item.frame, item.capture_index);
                        r->submitted = r->recorder->submitted();
                    }
                    r->recorder->close();
                } catch (const std::exception& e) { fail(r, r->config.name + ": video write: " + e.what()); }
            });
            r->reader = std::thread([&, r] {
                try {
                    r->camera->start();
                    auto last_received = std::chrono::steady_clock::now();
                    std::uint64_t previous_id = 0; bool previous_id_valid = false;
                    while (!stop) {
                        Frame f;
                        if (!r->camera->read(f, 200)) {
                            ++r->timeouts;
                            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - last_received).count() > config.no_frame_timeout_seconds)
                                throw std::runtime_error("No frames before timeout; check cable, exposure and external trigger");
                            continue;
                        }
                        last_received = std::chrono::steady_clock::now();
                        if (f.image.empty()) throw std::runtime_error("Camera returned an empty image");
                        if (f.frame_id_valid && previous_id_valid && f.frame_id > previous_id && f.frame_id - previous_id > 1)
                            r->gaps += f.frame_id - previous_id - 1;
                        if (f.frame_id_valid && previous_id_valid && f.frame_id <= previous_id)
                            ++r->discontinuities; // duplicate, reset or counter wrap; don't guess missing count
                        previous_id = f.frame_id; previous_id_valid = f.frame_id_valid;
                        const auto index = r->captured.fetch_add(1);
                        if (config.preview) { std::lock_guard<std::mutex> lock(r->mutex); r->latest = f.image; }
                        if (config.record && !r->queue.push({std::move(f), index}))
                            throw std::runtime_error("Recording queue full: stopping instead of silently dropping frames; reduce frame rate or improve encoding/storage throughput");
                        if (config.frame_limit && index + 1 >= config.frame_limit) break;
                    }
                } catch (const std::exception& e) { fail(r, r->config.name + ": capture: " + e.what()); }
                r->camera->stop();
                r->queue.close(); // writer continues draining after acquisition stops
                r->done = true;
            });
        }
        auto next_report = std::chrono::steady_clock::now();
        auto next_preview = next_report;
        while (!stop) {
            if (interrupted) { stop = true; break; }
            const auto now = std::chrono::steady_clock::now();
            if (config.duration_seconds && std::chrono::duration<double>(now - capture_start).count() >= config.duration_seconds) {
                stop = true; break;
            }
            bool all_done = true;
            for (const auto& r : runs) if (!r->done) all_done = false;
            if (all_done) break;
            if (now >= next_report) {
                for (const auto& r : runs) std::cout << r->config.name << " received=" << r->captured
                    << " submitted=" << r->submitted << " gaps=" << r->gaps << "  ";
                std::cout << '\n'; next_report = now + std::chrono::seconds(1);
            }
#ifdef MIAS_WITH_PREVIEW
            if (config.preview && now >= next_preview) {
                for (auto& r : runs) {
                    cv::Mat image; { std::lock_guard<std::mutex> lock(r->mutex); image = r->latest; }
                    if (!image.empty()) cv::imshow(r->config.name, image);
                }
                if (cv::waitKey(1) == 'q') { interrupted = 1; stop = true; }
                next_preview = now + std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 / config.preview_fps));
            }
#else
            (void)next_preview;
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } catch (const std::exception& e) { fail(nullptr, e.what()); }
    stop = true;
    for (auto& r : runs) {
        if (r->reader.joinable()) r->reader.join();
        else r->camera->stop();
        r->queue.close();
    }
    for (auto& r : runs) if (r->writer.joinable()) r->writer.join();
#ifdef MIAS_WITH_PREVIEW
    if (config.preview) cv::destroyAllWindows();
#endif
    manifest["cameras"] = json::array();
    manifest["integrity_warnings"] = json::array();
    for (auto& r : runs) {
        // Catch finalization failures even when a writer failed before close().
        if (r->recorder) {
            try { r->recorder->close(); } catch (const std::exception& e) { fail(r.get(), r->config.name + ": finalize: " + e.what()); }
            r->submitted = r->recorder->submitted();
        }
        if (!r->captured && session_error.empty()) fail(r.get(), r->config.name + ": no frames captured");
        if (config.record && r->captured != r->submitted && session_error.empty())
            fail(r.get(), r->config.name + ": received/submitted frame counts differ");
        if (r->gaps || r->discontinuities) {
            const auto warning = r->config.name + ": device frame IDs have gaps or discontinuities; inspect frames.csv";
            manifest["integrity_warnings"].push_back(warning);
            std::cerr << "WARNING: " << warning << '\n';
        }
        manifest["cameras"].push_back(camera_summary(*r));
    }
    manifest["status"] = !session_error.empty() ? "error" : interrupted ? "interrupted" : "completed";
    manifest["error"] = session_error;
    manifest["finished_unix_ns"] = unix_ns();
    manifest["elapsed_seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - session_start).count();
    save_json_atomic(session / "manifest.json", manifest);
    std::cout << "Finished: " << manifest["status"] << "; manifest=" << fs::absolute(session / "manifest.json") << '\n';
    if (!session_error.empty()) return 1;
    return interrupted ? 130 : 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::string config_path, output, backend;
        bool list = false, no_record = false, preview = false, duration_set = false, frames_set = false;
        int simulated = 0; double duration = 0; std::uint64_t frames = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&]() -> std::string {
                if (++i >= argc) throw std::runtime_error("Missing value for " + arg);
                return argv[i];
            };
            if (arg == "--help" || arg == "-h") { help(); return 0; }
            else if (arg == "--config") config_path = value();
            else if (arg == "--output") output = value();
            else if (arg == "--backend") backend = value();
            else if (arg == "--simulate") { const auto n = integer_arg(value()); if (n < 1 || n > 40) throw std::runtime_error("--simulate requires 1..40"); simulated = static_cast<int>(n); }
            else if (arg == "--frames") { frames = integer_arg(value()); frames_set = true; }
            else if (arg == "--duration") { duration = real_arg(value()); duration_set = true; }
            else if (arg == "--list") list = true;
            else if (arg == "--no-record") no_record = true;
            else if (arg == "--preview") preview = true;
            else throw std::runtime_error("Unknown argument: " + arg);
        }
        if (list) {
            if (!backend.empty() && backend != "hikrobot" && backend != "flir") throw std::runtime_error("--backend must be hikrobot or flir");
            std::vector<DeviceInfo> devices;
            auto append = [&](auto found) { devices.insert(devices.end(), found.begin(), found.end()); };
            bool any_backend = false;
#ifdef MIAS_WITH_HIKROBOT
            if (backend.empty() || backend == "hikrobot") { append(enumerate_hikrobot()); any_backend = true; }
#else
            if (backend == "hikrobot") throw std::runtime_error("Build with MIAS_WITH_HIKROBOT=ON first");
#endif
#ifdef MIAS_WITH_FLIR
            if (backend.empty() || backend == "flir") { append(enumerate_flir()); any_backend = true; }
#else
            if (backend == "flir") throw std::runtime_error("Build with MIAS_WITH_FLIR=ON first");
#endif
            (void)append;
            if (!any_backend) throw std::runtime_error("No hardware backends compiled. Enable MVS/Spinnaker or use --simulate");
            for (const auto& d : devices) std::cout << d.backend << '\t' << d.serial << '\t' << d.model << '\n';
            if (devices.empty()) std::cout << "No cameras found\n";
            return 0;
        }
        if (!backend.empty()) throw std::runtime_error("--backend is only used with --list");
        if (simulated && !config_path.empty()) throw std::runtime_error("Use --simulate or --config, not both");
        if (!simulated && config_path.empty()) { help(); return 2; }
        Config c = simulated ? simulated_config(simulated) : load_config(config_path);
        if (!output.empty()) c.output_dir = output;
        if (duration_set) c.duration_seconds = duration;
        if (frames_set) c.frame_limit = frames;
        if (no_record) c.record = false;
        if (preview) c.preview = true;
        std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
        return run(c);
    } catch (const std::exception& e) { std::cerr << "MIAS: " << e.what() << '\n'; return 1; }
}
