#include "mias/recorder.hpp"
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace mias {
namespace fs = std::filesystem;
Recorder::Recorder(const Config& c, const CameraConfig& camera, const fs::path& directory)
    : config_(c), camera_(camera), dir_(directory) {
    if (!fs::create_directory(dir_)) throw std::runtime_error("Output directory already exists: " + dir_.string());
    csv_.exceptions(std::ios::badbit | std::ios::failbit);
    csv_.open(dir_ / "frames.csv", std::ios::out | std::ios::trunc);
    csv_ << "video_file,video_frame_index,capture_index,frame_id,frame_id_valid,camera_timestamp,camera_timestamp_unit,host_monotonic_ns,host_unix_ns\n";
    csv_.flush();
}
Recorder::~Recorder() { try { close(); } catch (...) {} }

void Recorder::begin_segment(const Frame& frame) {
    if (frame.image.empty() || (frame.image.type() != CV_8UC1 && frame.image.type() != CV_8UC3))
        throw std::runtime_error("Recorder requires a nonempty Mono8/Bayer8/BGR8 image");
    if (frame.image.cols % 2 || frame.image.rows % 2)
        throw std::runtime_error("Odd frame dimensions would be truncated by video encoding; use an even ROI");
    if (type_ == -1) { size_ = frame.image.size(); type_ = frame.image.type(); }
    std::ostringstream name;
    name << "segment_" << std::setw(6) << std::setfill('0') << ++segment_ << '.' << config_.container;
    filename_ = name.str();
    const auto path = dir_ / filename_;
    if (fs::exists(path)) throw std::runtime_error("Refusing to overwrite " + path.string());
    const int codec = cv::VideoWriter::fourcc(config_.codec[0], config_.codec[1], config_.codec[2], config_.codec[3]);
    // Pin FFmpeg rather than silently falling back to an image sequence backend.
    if (!video_.open(path.string(), cv::CAP_FFMPEG, codec, camera_.fps, size_, type_ == CV_8UC3))
        throw std::runtime_error("Cannot open video encoder " + config_.codec + " for " + path.string() +
            "; install OpenCV with FFmpeg support");
    segment_frames_ = 0;
    segment_start_ns_ = frame.host_monotonic_ns;
}
void Recorder::finish_segment() {
    if (!video_.isOpened()) return;
    video_.release();
    const auto bytes = fs::file_size(dir_ / filename_);
    if (bytes == 0) throw std::runtime_error("Video file is empty: " + filename_);
    segments_.push_back({{"file", filename_}, {"frames_submitted", segment_frames_}, {"bytes", bytes}});
}
void Recorder::write(const Frame& frame, std::uint64_t capture_index) {
    if (type_ != -1 && (frame.image.type() != type_ || frame.image.size() != size_))
        throw std::runtime_error("Frame size/pixel layout changed during recording");
    if (video_.isOpened() && segment_frames_ && frame.host_monotonic_ns >= segment_start_ns_ &&
        (frame.host_monotonic_ns - segment_start_ns_) / 1e9 >= config_.segment_seconds)
        finish_segment();
    if (!video_.isOpened()) begin_segment(frame);
    video_.write(frame.image);
    // OpenCV's write() returns void: these are encoder submissions, not a claim
    // that every packet reached durable storage. Integration tests decode files.
    ++total_frames_;
    csv_ << filename_ << ',' << segment_frames_++ << ',' << capture_index << ','
         << frame.frame_id << ',' << (frame.frame_id_valid ? 1 : 0) << ','
         << frame.camera_timestamp << ',' << frame.camera_timestamp_unit << ','
         << frame.host_monotonic_ns << ',' << frame.host_unix_ns << '\n';
    if (total_frames_ % 30 == 0) csv_.flush();
}
void Recorder::close() {
    finish_segment();
    if (csv_.is_open()) { csv_.flush(); csv_.close(); }
}
void save_json_atomic(const fs::path& path, const nlohmann::json& value) {
    const fs::path temporary = path.string() + ".tmp";
    std::ofstream out;
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.open(temporary, std::ios::out | std::ios::trunc);
    out << value.dump(2) << '\n'; out.close();
    fs::rename(temporary, path);
}
} // namespace mias
