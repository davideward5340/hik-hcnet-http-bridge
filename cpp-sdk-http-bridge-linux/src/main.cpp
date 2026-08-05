#include "HCNetSDK.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::atomic_bool g_running{true};

void on_signal(int) { g_running.store(false); }

class DailyLogWriter {
public:
    DailyLogWriter(fs::path directory, int retention_days) : directory_(std::move(directory)), retention_days_(retention_days < 1 ? 30 : retention_days) {
        std::error_code error;
        fs::create_directories(directory_, error);
        const auto now = std::time(nullptr);
        cleanup(now);
        last_cleanup_date_ = day_key(now);
    }

    void write(const std::string& line, std::time_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string date = day_key(now);
        if (date != last_cleanup_date_) {
            cleanup(now);
            last_cleanup_date_ = date;
        }
        std::error_code error;
        fs::create_directories(directory_, error);
        std::ofstream output(directory_ / (date + ".log"), std::ios::out | std::ios::app);
        if (output) output << line << '\n';
    }

private:
    static std::tm local_time(std::time_t now) {
        std::tm value{};
        localtime_r(&now, &value);
        return value;
    }

    static std::string day_key(std::time_t now) {
        const auto value = local_time(now);
        std::ostringstream text;
        text << std::put_time(&value, "%F");
        return text.str();
    }

    static bool is_daily_log_name(const std::string& name) {
        if (name.size() != 14 || name.substr(10) != ".log" || name[4] != '-' || name[7] != '-') return false;
        for (size_t index = 0; index < 10; ++index) {
            if (index == 4 || index == 7) continue;
            if (!std::isdigit(static_cast<unsigned char>(name[index]))) return false;
        }
        return true;
    }

    void cleanup(std::time_t now) {
        auto cutoff = local_time(now);
        cutoff.tm_hour = 12;
        cutoff.tm_min = 0;
        cutoff.tm_sec = 0;
        cutoff.tm_mday -= retention_days_;
        const std::time_t cutoff_time = std::mktime(&cutoff);
        const std::string cutoff_date = day_key(cutoff_time);
        std::error_code error;
        for (fs::directory_iterator item(directory_, error); !error && item != fs::directory_iterator(); item.increment(error)) {
            const auto name = item->path().filename().string();
            if (!item->is_regular_file(error) || item->path().extension() != ".log") continue;
            bool expired = is_daily_log_name(name) && name.substr(0, 10) < cutoff_date;
            if (!expired && !is_daily_log_name(name)) {
                std::error_code time_error;
                const auto modified = fs::last_write_time(item->path(), time_error);
                expired = !time_error && fs::file_time_type::clock::now() - modified > std::chrono::hours(24 * retention_days_);
            }
            if (expired) {
                std::error_code remove_error;
                fs::remove(item->path(), remove_error);
            }
        }
    }

    fs::path directory_;
    int retention_days_{30};
    std::mutex mutex_;
    std::string last_cleanup_date_;
};

std::unique_ptr<DailyLogWriter> g_daily_logger;

void log_line(const char* level, const std::string& text) {
    const auto now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    const char* chinese_level = std::strcmp(level, "WARN") == 0 ? "警告" :
        (std::strcmp(level, "ERROR") == 0 ? "错误" : "信息");
    std::ostringstream line;
    line << std::put_time(&tm, "%F %T") << " [" << chinese_level << "] " << text;
    std::cerr << line.str() << std::endl;
    if (g_daily_logger) g_daily_logger->write(line.str(), now);
}

struct BridgeError final : std::runtime_error {
    int http_status;
    std::string code;
    BridgeError(int status, std::string error_code, const std::string& message)
        : std::runtime_error(message), http_status(status), code(std::move(error_code)) {}
};

struct ClientDisconnected final : std::runtime_error {
    ClientDisconnected() : std::runtime_error("HTTP client disconnected") {}
};

struct Config {
    std::string bind{"127.0.0.1"};
    int port{28080};
    int max_sessions{8};
    fs::path sdk_directory{"../sdk"};
    // 支持时在正式取流前读取设备编码参数；不支持的老设备自动回退到实际码流探测。
    bool read_video_codec_from_device{true};
    // 实时取流成功后请求设备立即发送 I 帧。失败仅记录日志，不中断播放。
    bool force_keyframe_on_realplay{true};
    // I 帧间隔（帧数），25 约等于 25fps 设备的一秒；0 表示不修改设备持久配置。
    int realplay_keyframe_interval_frames{25};
    // 在登录 HCNetSDK 前探测 NVR SDK TCP 端口，快速区分网络不可达和认证/通道错误。
    int connect_probe_timeout_ms{1500};
    fs::path ffmpeg_path{"/usr/bin/ffmpeg"};
    std::string ffmpeg_log_level{"info"};
    fs::path log_directory{"../logs"};
    int log_retention_days{30};
    size_t queue_bytes{64 * 1024 * 1024};
    int queue_backpressure_ms{500};
    int max_playback_seconds{86400};
    int sdk_start_ms{10000};
    int first_media_ms{15000};
    int no_sdk_data_ms{10000};
    // SDK 回放数据回调可能因短暂背压阻塞。按 SDK 建议以小于 2 秒的间隔发送保活。
    int playback_keep_alive_ms{1500};
    std::string output_video_codec{"h264"};
    std::string video_encoder{"libx264"};
    std::string video_preset{"veryfast"};
    // 默认仅输出视频，降低桥接到浏览器的带宽与 AAC 编码开销。
    bool enable_audio{false};
    // 仅缓存编码判断；不保存认证信息。0 表示每次都重新探测。
    int codec_cache_seconds{900};
    fs::path codec_cache_file{"../state/video-codec-cache.tsv"};
};

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path);
    if (!input) throw BridgeError(500, "CONFIG_NOT_FOUND", "configuration file not found: " + path.string());
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

std::string json_section(const std::string& json, const std::string& name) {
    const std::string key = "\"" + name + "\"";
    const auto key_at = json.find(key);
    if (key_at == std::string::npos) return {};
    const auto open = json.find('{', key_at + key.size());
    if (open == std::string::npos) return {};
    int depth = 0;
    bool quoted = false;
    for (size_t i = open; i < json.size(); ++i) {
        if (json[i] == '"' && (i == 0 || json[i - 1] != '\\')) quoted = !quoted;
        if (quoted) continue;
        if (json[i] == '{') ++depth;
        if (json[i] == '}' && --depth == 0) return json.substr(open + 1, i - open - 1);
    }
    return {};
}

std::optional<std::string> json_value(const std::string& section, const std::string& name) {
    const std::string key = "\"" + name + "\"";
    const auto key_at = section.find(key);
    if (key_at == std::string::npos) return std::nullopt;
    auto at = section.find(':', key_at + key.size());
    if (at == std::string::npos) return std::nullopt;
    ++at;
    while (at < section.size() && std::isspace(static_cast<unsigned char>(section[at]))) ++at;
    if (at >= section.size()) return std::nullopt;
    if (section[at] == '"') {
        const auto end = section.find('"', at + 1);
        if (end == std::string::npos) return std::nullopt;
        return section.substr(at + 1, end - at - 1);
    }
    const auto end = section.find_first_of(",}\r\n", at);
    return trim(section.substr(at, end == std::string::npos ? std::string::npos : end - at));
}

int json_int(const std::string& section, const std::string& name, int fallback) {
    const auto value = json_value(section, name);
    if (!value) return fallback;
    try { return std::stoi(*value); }
    catch (...) { throw BridgeError(500, "INVALID_CONFIG", "invalid integer config value: " + name); }
}

bool json_bool(const std::string& section, const std::string& name, bool fallback) {
    const auto value = json_value(section, name);
    if (!value) return fallback;
    if (*value == "true") return true;
    if (*value == "false") return false;
    throw BridgeError(500, "INVALID_CONFIG", "invalid boolean config value: " + name);
}

std::string json_string(const std::string& section, const std::string& name, const std::string& fallback) {
    const auto value = json_value(section, name);
    return value ? *value : fallback;
}

Config load_config(const fs::path& config_path) {
    const auto json = read_file(config_path);
    Config config;
    const auto sdk_type = json_string(json, "sdktype", "hcnetsdk");
    const auto server = json_section(json, "server");
    const auto sdk = json_section(json, "sdk");
    const auto ffmpeg = json_section(json, "ffmpeg");
    const auto logging = json_section(json, "logging");
    const auto media = json_section(json, "media");
    const auto timeouts = json_section(json, "timeouts");
    const auto root = config_path.parent_path();
    config.bind = json_string(server, "bind", config.bind);
    config.port = json_int(server, "port", config.port);
    config.max_sessions = json_int(server, "maxSessions", config.max_sessions);
    config.sdk_directory = json_string(sdk, "directory", config.sdk_directory.string());
    config.read_video_codec_from_device = json_bool(sdk, "readVideoCodecFromDevice", config.read_video_codec_from_device);
    config.force_keyframe_on_realplay = json_bool(sdk, "forceKeyFrameOnRealPlay", config.force_keyframe_on_realplay);
    config.realplay_keyframe_interval_frames = json_int(sdk, "realPlayKeyFrameIntervalFrames", config.realplay_keyframe_interval_frames);
    config.connect_probe_timeout_ms = json_int(sdk, "connectProbeTimeoutMs", config.connect_probe_timeout_ms);
    config.ffmpeg_path = json_string(ffmpeg, "path", config.ffmpeg_path.string());
    config.ffmpeg_log_level = json_string(ffmpeg, "logLevel", config.ffmpeg_log_level);
    config.log_directory = json_string(logging, "directory", config.log_directory.string());
    config.log_retention_days = json_int(logging, "retentionDays", config.log_retention_days);
    const int queue_bytes = json_int(media, "queueBytes", static_cast<int>(config.queue_bytes));
    if (queue_bytes < 0) throw BridgeError(500, "INVALID_CONFIG", "media.queueBytes must be positive");
    config.queue_bytes = static_cast<size_t>(queue_bytes);
    config.queue_backpressure_ms = json_int(media, "queueBackpressureMs", config.queue_backpressure_ms);
    config.max_playback_seconds = json_int(media, "maxPlaybackSeconds", config.max_playback_seconds);
    config.output_video_codec = json_string(media, "outputVideoCodec", config.output_video_codec);
    config.video_encoder = json_string(media, "videoEncoder", config.video_encoder);
    config.video_preset = json_string(media, "videoPreset", config.video_preset);
    config.enable_audio = json_bool(media, "enableAudio", config.enable_audio);
    config.codec_cache_seconds = json_int(media, "codecCacheSeconds", config.codec_cache_seconds);
    config.codec_cache_file = json_string(media, "codecCacheFile", config.codec_cache_file.string());
    config.sdk_start_ms = json_int(timeouts, "sdkStartMs", config.sdk_start_ms);
    config.first_media_ms = json_int(timeouts, "firstMediaMs", config.first_media_ms);
    config.no_sdk_data_ms = json_int(timeouts, "noSdkDataMs", config.no_sdk_data_ms);
    config.playback_keep_alive_ms = json_int(timeouts, "playbackKeepAliveMs", config.playback_keep_alive_ms);
    if (config.sdk_directory.is_relative()) config.sdk_directory = fs::absolute(root / config.sdk_directory);
    if (config.codec_cache_file.is_relative()) config.codec_cache_file = fs::absolute(root / config.codec_cache_file);
    if (config.ffmpeg_path.is_relative()) config.ffmpeg_path = fs::absolute(root / config.ffmpeg_path);
    if (config.log_directory.is_relative()) config.log_directory = fs::absolute(root / config.log_directory);
    // 本地开发或非标准镜像布局可通过环境变量覆盖，不影响默认 OCI 配置。
    if (const char* sdk_override = std::getenv("HIK_BRIDGE_SDK_DIR")) config.sdk_directory = sdk_override;
    if (const char* ffmpeg_override = std::getenv("HIK_BRIDGE_FFMPEG_PATH")) config.ffmpeg_path = ffmpeg_override;
    if (const char* log_override = std::getenv("HIK_BRIDGE_LOG_DIR")) config.log_directory = log_override;
    if (sdk_type != "hcnetsdk" || config.bind.empty() || config.bind == "0.0.0.0" || config.port < 1 || config.port > 65535 || config.max_sessions < 1 || config.queue_bytes < 65536 || config.queue_backpressure_ms < 0 || config.queue_backpressure_ms > 5000 || config.codec_cache_seconds < 0 || config.codec_cache_seconds > 86400 || config.realplay_keyframe_interval_frames < 0 || config.realplay_keyframe_interval_frames > 65535 || config.connect_probe_timeout_ms < 100 || config.connect_probe_timeout_ms > 10000 || config.codec_cache_file.empty() || config.log_retention_days < 1 ||
        config.max_playback_seconds < 1 || config.sdk_start_ms < 1 || config.first_media_ms < 1 || config.no_sdk_data_ms < 1 ||
        config.playback_keep_alive_ms < 1000 || config.playback_keep_alive_ms > 5000 ||
        config.log_directory.empty() || config.output_video_codec != "h264" || config.video_encoder.empty() || config.video_preset.empty()) {
        throw BridgeError(500, "INVALID_CONFIG", "invalid bridge configuration");
    }
    return config;
}

std::string url_decode(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') result.push_back(' ');
        else if (value[i] == '%' && i + 2 < value.size() && std::isxdigit(value[i + 1]) && std::isxdigit(value[i + 2])) {
            result.push_back(static_cast<char>(std::strtol(value.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else result.push_back(value[i]);
    }
    return result;
}

using Query = std::map<std::string, std::string>;

Query parse_query(const std::string& text) {
    Query query;
    size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('&', start);
        const auto part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto equals = part.find('=');
        query[url_decode(part.substr(0, equals))] = url_decode(equals == std::string::npos ? "" : part.substr(equals + 1));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return query;
}

std::string required(const Query& query, const std::string& name) {
    const auto item = query.find(name);
    if (item == query.end() || item->second.empty()) throw BridgeError(400, "INVALID_PARAMETER", name + " is required");
    return item->second;
}

long long parse_long(const std::string& value, const std::string& name) {
    try { size_t used = 0; const auto result = std::stoll(value, &used); if (used != value.size()) throw std::invalid_argument("tail"); return result; }
    catch (...) { throw BridgeError(400, "INVALID_PARAMETER", name + " must be an integer"); }
}

int parse_int(const std::string& value, const std::string& name) {
    const auto result = parse_long(value, name);
    if (result < INT32_MIN || result > INT32_MAX) throw BridgeError(400, "INVALID_PARAMETER", name + " is out of range");
    return static_cast<int>(result);
}

enum class Option { RealPlay, Playback };
enum class Stream { Main, Sub };
enum class DetectedVideoCodec { Unknown, H264, H265 };
const char* video_codec_name(DetectedVideoCodec codec);
// analog/digital 的 camera 是真实 HCNetSDK 通道号；digitalIndex 的 camera 是
// 上层存储的零基数字通道序号，由 Bridge 根据设备 dwStartDChan 动态换算。
enum class ChannelType { Auto, Analog, Digital, DigitalIndex };

struct StreamRequest {
    Option option;
    std::string ip;
    int port;
    std::string username;
    std::string password;
    int camera;
    ChannelType channel_type{ChannelType::Auto};
    Stream stream;
    double speed;
    long long start{0};
    long long end{0};
    std::string sid;
};

StreamRequest parse_stream_request(const Query& query, const Config& config) {
    StreamRequest request{};
    const auto option = required(query, "option");
    if (option == "realplay") request.option = Option::RealPlay;
    else if (option == "playback") request.option = Option::Playback;
    else throw BridgeError(400, "INVALID_PARAMETER", "option must be realplay or playback");
    request.ip = required(query, "ip");
    request.port = parse_int(required(query, "port"), "port");
    request.username = required(query, "username");
    request.password = required(query, "password");
    request.camera = parse_int(required(query, "camera"), "camera");
    request.sid = required(query, "sid");
    const auto channel_type = query.count("channelType") ? query.at("channelType") : "auto";
    if (channel_type == "auto") request.channel_type = ChannelType::Auto;
    else if (channel_type == "analog") request.channel_type = ChannelType::Analog;
    else if (channel_type == "digital" || channel_type == "ip") request.channel_type = ChannelType::Digital;
    else if (channel_type == "digitalIndex") request.channel_type = ChannelType::DigitalIndex;
    else throw BridgeError(400, "INVALID_PARAMETER", "channelType must be auto, analog, digital, digitalIndex or ip");
    const auto stream = query.count("stream") ? query.at("stream") : "main";
    if (stream == "main") request.stream = Stream::Main;
    else if (stream == "sub") request.stream = Stream::Sub;
    else throw BridgeError(400, "INVALID_PARAMETER", "stream must be main or sub");
    const auto speed_text = query.count("speed") ? query.at("speed") : "1";
    try { request.speed = std::stod(speed_text); } catch (...) { throw BridgeError(400, "INVALID_PARAMETER", "unsupported speed"); }
    const std::set<double> valid_speeds{16, 8, 4, 2, 1, .5, .25, .125, .0625};
    if (!valid_speeds.count(request.speed)) throw BridgeError(400, "INVALID_PARAMETER", "unsupported speed");
    if (request.port < 1 || request.port > 65535 || request.camera < 0 || (request.channel_type != ChannelType::DigitalIndex && request.camera < 1) || request.ip.size() > 128 || request.username.size() > 63 || request.password.size() > 63 || request.sid.size() > 128) {
        throw BridgeError(400, "INVALID_PARAMETER", "NVR parameter is out of range");
    }
    if (request.option == Option::Playback) {
        request.start = parse_long(required(query, "start"), "start");
        request.end = parse_long(required(query, "end"), "end");
        if (request.start < 0 || request.end <= request.start || request.end - request.start > config.max_playback_seconds) {
            throw BridgeError(400, "INVALID_PARAMETER", "invalid playback time range");
        }
    } else if (request.speed != 1) {
        throw BridgeError(400, "INVALID_PARAMETER", "realplay only supports speed=1");
    }
    return request;
}

class ByteQueue {
public:
    explicit ByteQueue(size_t limit) : limit_(limit) {}
    // SDK 回调只进行有上限的短暂等待：可吸收瞬时拥塞，又不会无限阻塞 HCNetSDK 的回调线程。
    bool push(std::vector<uint8_t> data, std::chrono::milliseconds backpressure_timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_ || data.empty() || data.size() > limit_) return false;
        const auto deadline = std::chrono::steady_clock::now() + backpressure_timeout;
        while (!closed_ && bytes_ + data.size() > limit_) {
            ++backpressure_events_;
            if (std::chrono::steady_clock::now() >= deadline || !cv_.wait_until(lock, deadline, [&] { return closed_ || bytes_ + data.size() <= limit_; })) return false;
        }
        if (closed_) return false;
        bytes_ += data.size();
        peak_bytes_ = std::max(peak_bytes_, bytes_);
        items_.push_back(std::move(data));
        cv_.notify_all();
        return true;
    }
    bool pop(std::vector<uint8_t>& data, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [&] { return closed_ || !items_.empty(); })) return false;
        if (items_.empty()) return false;
        data = std::move(items_.front());
        bytes_ -= data.size();
        items_.pop_front();
        cv_.notify_all();
        return true;
    }
    bool closed() const { std::lock_guard<std::mutex> lock(mutex_); return closed_ && items_.empty(); }
    size_t current_bytes() const { std::lock_guard<std::mutex> lock(mutex_); return bytes_; }
    size_t peak_bytes() const { std::lock_guard<std::mutex> lock(mutex_); return peak_bytes_; }
    size_t backpressure_events() const { std::lock_guard<std::mutex> lock(mutex_); return backpressure_events_; }
    void close() { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; cv_.notify_all(); }
private:
    const size_t limit_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> items_;
    size_t bytes_{0};
    size_t peak_bytes_{0};
    size_t backpressure_events_{0};
    bool closed_{false};
};

// 与 Windows C# 版本一致：Unix 秒先按桥接机本地时区转换，再传给 NVR。
NET_DVR_TIME unix_to_nvr_time(long long seconds) {
    const std::time_t local_seconds = static_cast<std::time_t>(seconds);
    std::tm value{};
    localtime_r(&local_seconds, &value);
    NET_DVR_TIME result{};
    result.dwYear = value.tm_year + 1900;
    result.dwMonth = value.tm_mon + 1;
    result.dwDay = value.tm_mday;
    result.dwHour = value.tm_hour;
    result.dwMinute = value.tm_min;
    result.dwSecond = value.tm_sec;
    return result;
}

enum class StreamEndReason : int {
    Active,
    PlaybackEnd,
    PlaybackPositionError,
    QueueOverflow,
    SdkDataTimeout,
    SdkKeepAliveFailed,
    FfmpegExit,
    ClientReplaced,
    ServerStopped,
    StartupFailed,
    StreamEnded
};

const char* end_reason_name(StreamEndReason reason) {
    switch (reason) {
        case StreamEndReason::PlaybackEnd: return "playback_end";
        case StreamEndReason::PlaybackPositionError: return "playback_position_error";
        case StreamEndReason::QueueOverflow: return "queue_overflow";
        case StreamEndReason::SdkDataTimeout: return "sdk_data_timeout";
        case StreamEndReason::SdkKeepAliveFailed: return "sdk_keepalive_failed";
        case StreamEndReason::FfmpegExit: return "ffmpeg_exit";
        case StreamEndReason::ClientReplaced: return "client_replaced";
        case StreamEndReason::ServerStopped: return "server_stopped";
        case StreamEndReason::StartupFailed: return "startup_failed";
        case StreamEndReason::StreamEnded: return "stream_ended";
        default: return "active";
    }
}

std::string end_reason_message(StreamEndReason reason) {
    switch (reason) {
        case StreamEndReason::PlaybackEnd: return "回放已到达请求的结束时间";
        case StreamEndReason::PlaybackPositionError: return "HCNetSDK 回放位置状态异常";
        case StreamEndReason::QueueOverflow: return "HCNetSDK 媒体队列在限定背压后仍持续满载";
        case StreamEndReason::SdkDataTimeout: return "HCNetSDK 未继续输出媒体数据";
        case StreamEndReason::SdkKeepAliveFailed: return "HCNetSDK 回放保活连续失败";
        case StreamEndReason::FfmpegExit: return "FFmpeg 在视频结束前退出";
        case StreamEndReason::ClientReplaced: return "浏览器主动停止或替换了视频流";
        case StreamEndReason::ServerStopped: return "桥接服务正在停止";
        case StreamEndReason::StartupFailed: return "视频流启动失败";
        default: return "视频流异常结束";
    }
}

class HcNetRuntime {
public:
    explicit HcNetRuntime(const Config& config) {
        const auto sdk = fs::absolute(config.sdk_directory);
        const auto crypto = sdk / "libcrypto.so.3";
        const auto ssl = sdk / "libssl.so.3";
        if (!fs::exists(sdk / "libhcnetsdk.so") || !fs::exists(crypto) || !fs::exists(ssl)) {
            throw BridgeError(500, "SDK_NOT_FOUND", "HCNetSDK libraries are incomplete in " + sdk.string());
        }
        NET_DVR_LOCAL_SDK_PATH sdk_path{};
        std::snprintf(sdk_path.sPath, sizeof(sdk_path.sPath), "%s", sdk.c_str());
        NET_DVR_SetSDKInitCfg(NET_SDK_INIT_CFG_SDK_PATH, &sdk_path);
        NET_DVR_SetSDKInitCfg(NET_SDK_INIT_CFG_LIBEAY_PATH, const_cast<char*>(crypto.c_str()));
        NET_DVR_SetSDKInitCfg(NET_SDK_INIT_CFG_SSLEAY_PATH, const_cast<char*>(ssl.c_str()));
        NET_DVR_SetConnectTime(3000, 2);
        NET_DVR_SetReconnect(10000, TRUE);
        if (!NET_DVR_Init()) throw BridgeError(500, "SDK_INIT_FAILED", "NET_DVR_Init failed, error=" + std::to_string(NET_DVR_GetLastError()));
        initialized_ = true;
        log_line("INFO", "HCNetSDK 初始化成功：目录=" + sdk.string() + "。");
    }
    ~HcNetRuntime() { if (initialized_) NET_DVR_Cleanup(); }
private:
    bool initialized_{false};
};

enum class ResolvedChannelType { Analog, Digital, Unknown };

const char* channel_type_name(ChannelType value) {
    switch (value) {
        case ChannelType::Analog: return "模拟通道";
        case ChannelType::Digital: return "数字通道";
        case ChannelType::DigitalIndex: return "数字通道序号";
        default: return "自动";
    }
}

const char* resolved_channel_type_name(ResolvedChannelType value) {
    switch (value) {
        case ResolvedChannelType::Analog: return "模拟通道";
        case ResolvedChannelType::Digital: return "数字通道";
        default: return "未识别";
    }
}

// 设备能力由 NET_DVR_Login_V40 返回的 DEVICEINFO_V40 和
// NET_DVR_GET_IPPARACFG_V40 共同给出。后者读取失败时仍保留前者，兼容旧设备。
class NvrChannelTopology {
public:
    static NvrChannelTopology read(LONG user, const NET_DVR_DEVICEINFO_V40& info, const std::string& sid) {
        NvrChannelTopology topology;
        const auto& device = info.struDeviceV30;
        topology.analog_start_ = device.byStartChan;
        topology.analog_count_ = device.byChanNum;
        topology.digital_start_ = device.byStartDChan;
        topology.digital_count_ = static_cast<int>(device.byIPChanNum) | (static_cast<int>(device.byHighDChanNum) << 8);
        if (topology.analog_count_ > 0 && topology.analog_start_ == 0) topology.analog_start_ = 1;
        for (int index = 0; index < MAX_CHANNUM_V30; ++index) topology.analog_enabled_[index] = index < topology.analog_count_;

        NET_DVR_IPPARACFG_V40 ip_config{};
        ip_config.dwSize = sizeof(ip_config);
        DWORD returned = 0;
        if (NET_DVR_GetDVRConfig(user, NET_DVR_GET_IPPARACFG_V40, 0, &ip_config, sizeof(ip_config), &returned)) {
            topology.ip_parameter_config_read_ = true;
            // 少数旧 DVR 的 V40 配置会把模拟数量返回为 0；此时保留登录信息，
            // 不能将一个真实的模拟通道错误判为不存在。
            if (ip_config.dwAChanNum > 0) topology.analog_count_ = std::max(topology.analog_count_, static_cast<int>(ip_config.dwAChanNum));
            if (ip_config.dwDChanNum > 0) topology.digital_count_ = std::max(topology.digital_count_, static_cast<int>(ip_config.dwDChanNum));
            if (ip_config.dwStartDChan > 0) topology.digital_start_ = static_cast<int>(ip_config.dwStartDChan);
            if (ip_config.dwAChanNum > 0) {
                for (int index = 0; index < MAX_CHANNUM_V30; ++index) topology.analog_enabled_[index] = ip_config.byAnalogChanEnable[index] != 0;
            }
        } else {
            log_line("WARN", "sid=" + sid + " 未读取到 NVR IP 通道配置，按登录返回的通道能力继续：HCNetSDK 错误=" + std::to_string(NET_DVR_GetLastError()) + "。");
        }
        log_line("INFO", "sid=" + sid + " NVR 通道能力已读取：模拟起始/数量=" + std::to_string(topology.analog_start_) + "/" + std::to_string(topology.analog_count_) + "，数字起始/数量=" + std::to_string(topology.digital_start_) + "/" + std::to_string(topology.digital_count_) + "。");
        return topology;
    }

    // digitalIndex 使用本项目后台的零基序号：真实通道 = dwStartDChan + 索引。
    int resolve(int camera, ChannelType requested_type, ResolvedChannelType& resolved_type) const {
        const bool analog = is_analog(camera);
        const bool digital = is_digital(camera);
        if (requested_type == ChannelType::Analog) {
            if (analog_count_ <= 0) throw unsupported_analog(camera);
            if (!analog) throw BridgeError(422, "ANALOG_CHANNEL_OUT_OF_RANGE", "camera=" + std::to_string(camera) + " is not in the NVR analog channel range " + channel_range(analog_start_, analog_count_));
            ensure_analog_enabled(camera);
            resolved_type = ResolvedChannelType::Analog;
            return camera;
        }
        if (requested_type == ChannelType::Digital) {
            if (digital_count_ <= 0) throw BridgeError(422, "DIGITAL_CHANNEL_UNSUPPORTED", "NVR reports no digital/IP channels");
            if (!digital) throw BridgeError(422, "DIGITAL_CHANNEL_OUT_OF_RANGE", "camera=" + std::to_string(camera) + " is not in the NVR digital/IP channel range " + channel_range(digital_start_, digital_count_));
            resolved_type = ResolvedChannelType::Digital;
            return camera;
        }
        if (requested_type == ChannelType::DigitalIndex) {
            if (digital_count_ <= 0) throw BridgeError(422, "DIGITAL_CHANNEL_UNSUPPORTED", "NVR reports no digital/IP channels");
            if (camera < 0 || camera >= digital_count_) throw BridgeError(422, "DIGITAL_CHANNEL_INDEX_OUT_OF_RANGE", "camera=" + std::to_string(camera) + " is not in the zero-based digital channel index range 0-" + std::to_string(digital_count_ - 1));
            resolved_type = ResolvedChannelType::Digital;
            return digital_start_ + camera;
        }
        if (analog) {
            ensure_analog_enabled(camera);
            resolved_type = ResolvedChannelType::Analog;
            return camera;
        }
        if (digital) {
            resolved_type = ResolvedChannelType::Digital;
            return camera;
        }
        throw BridgeError(422, "CHANNEL_NOT_FOUND", "camera=" + std::to_string(camera) + " is not reported by this NVR; analog=" + channel_range(analog_start_, analog_count_) + ", digital=" + channel_range(digital_start_, digital_count_));
    }

private:
    bool is_analog(int camera) const { return analog_count_ > 0 && camera >= analog_start_ && camera < analog_start_ + analog_count_; }
    bool is_digital(int camera) const { return digital_count_ > 0 && camera >= digital_start_ && camera < digital_start_ + digital_count_; }
    void ensure_analog_enabled(int camera) const {
        const int index = camera - analog_start_;
        if (ip_parameter_config_read_ && (index < 0 || index >= MAX_CHANNUM_V30 || !analog_enabled_[static_cast<size_t>(index)])) {
            throw BridgeError(422, "ANALOG_CHANNEL_DISABLED", "camera=" + std::to_string(camera) + " is an analog channel but is disabled or has no analog input configured on the NVR");
        }
    }
    BridgeError unsupported_analog(int camera) const {
        const auto start = digital_start_ > 0 ? std::to_string(digital_start_) : "33";
        return BridgeError(422, "ANALOG_CHANNEL_UNSUPPORTED", "camera=" + std::to_string(camera) + " requested as analog, but this NVR reports no analog channels; use the NVR digital/IP SDK channel number (often starting at " + start + ") for an IP camera");
    }
    int enabled_analog_count() const {
        int result = 0;
        for (int index = 0; index < analog_count_ && index < MAX_CHANNUM_V30; ++index) if (analog_enabled_[static_cast<size_t>(index)]) ++result;
        return result;
    }
    static std::string channel_range(int start, int count) {
        return count <= 0 || start <= 0 ? "none" : std::to_string(start) + "-" + std::to_string(start + count - 1);
    }

    int analog_start_{0};
    int analog_count_{0};
    int digital_start_{0};
    int digital_count_{0};
    std::array<bool, MAX_CHANNUM_V30> analog_enabled_{};
    bool ip_parameter_config_read_{false};
};

// 在调用 HCNetSDK 前确认私有 SDK 端口可以建立 TCP 连接，避免将 IP、端口、路由或防火墙问题笼统报为登录失败。
void probe_nvr_endpoint(const StreamRequest& request, int timeout_ms) {
    const auto started = std::chrono::steady_clock::now();
    const std::string target = request.ip + ":" + std::to_string(request.port);
    log_line("INFO", "sid=" + request.sid + " 开始检测 NVR SDK 端口可达性：设备=" + target + "，超时=" + std::to_string(timeout_ms) + "ms。");

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const int resolve_error = ::getaddrinfo(request.ip.c_str(), std::to_string(request.port).c_str(), &hints, &addresses);
    if (resolve_error != 0) {
        log_line("WARN", "sid=" + request.sid + " NVR SDK 地址解析失败：设备=" + target + "，原因=" + ::gai_strerror(resolve_error) + "。");
        throw BridgeError(502, "NVR_UNREACHABLE", "无法连接 NVR " + target + " 的 SDK 端口；请检查设备 IP、SDK 端口、网络连通性或防火墙。");
    }

    int last_error = ETIMEDOUT;
    bool connected = false;
    for (addrinfo* item = addresses; item != nullptr && !connected; item = item->ai_next) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        const int remaining = timeout_ms - static_cast<int>(elapsed);
        if (remaining <= 0) break;

        const int fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0) {
            last_error = errno;
            continue;
        }
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            last_error = errno;
            ::close(fd);
            continue;
        }
        if (::connect(fd, item->ai_addr, item->ai_addrlen) == 0) {
            connected = true;
            ::close(fd);
            break;
        }
        if (errno == EINPROGRESS) {
            pollfd poll_fd{};
            poll_fd.fd = fd;
            poll_fd.events = POLLOUT;
            const int poll_result = ::poll(&poll_fd, 1, remaining);
            if (poll_result > 0) {
                socklen_t length = sizeof(last_error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &last_error, &length) == 0 && last_error == 0) connected = true;
            } else {
                last_error = poll_result == 0 ? ETIMEDOUT : errno;
            }
        } else {
            last_error = errno;
        }
        ::close(fd);
    }
    ::freeaddrinfo(addresses);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (connected) {
        log_line("INFO", "sid=" + request.sid + " NVR SDK 端口可达：设备=" + target + "，耗时=" + std::to_string(elapsed) + "ms。");
        return;
    }
    log_line("WARN", "sid=" + request.sid + " NVR SDK 端口不可达：设备=" + target + "，系统错误=" + std::to_string(last_error) + "，耗时=" + std::to_string(elapsed) + "ms。");
    throw BridgeError(502, "NVR_UNREACHABLE", "无法连接 NVR " + target + " 的 SDK 端口；请检查设备 IP、SDK 端口、网络连通性或防火墙。");
}

class HcSession {
public:
    HcSession(const StreamRequest& request, const Config& config)
        : request_(request), config_(config), queue_(config.queue_bytes) {}
    ~HcSession() { stop(); }
    ByteQueue& queue() { return queue_; }
    StreamEndReason end_reason() const { return end_reason_.load(); }
    size_t queue_current_bytes() const { return queue_.current_bytes(); }
    size_t queue_peak_bytes() const { return queue_.peak_bytes(); }
    size_t queue_backpressure_events() const { return queue_.backpressure_events(); }
    DetectedVideoCodec configured_video_codec() const { return configured_video_codec_; }
    bool stream_started() const { return stream_started_; }

    void start() {
        prepare();
        start_stream_and_wait_for_data();
    }

    // 登录、通道解析和设备编码参数读取先于实际取流执行，以便 Bridge 在创建 FFmpeg 前选择正确管线。
    void prepare() {
        if (prepared_) return;
        probe_nvr_endpoint(request_, config_.connect_probe_timeout_ms);
        const auto login_started = std::chrono::steady_clock::now();
        log_line("INFO", "sid=" + request_.sid + " 开始登录 NVR：设备=" + request_.ip + ":" + std::to_string(request_.port) + "。");
        NET_DVR_USER_LOGIN_INFO login{};
        std::snprintf(login.sDeviceAddress, sizeof(login.sDeviceAddress), "%s", request_.ip.c_str());
        std::snprintf(reinterpret_cast<char*>(login.sUserName), sizeof(login.sUserName), "%s", request_.username.c_str());
        std::snprintf(reinterpret_cast<char*>(login.sPassword), sizeof(login.sPassword), "%s", request_.password.c_str());
        login.wPort = static_cast<WORD>(request_.port);
        login.bUseAsynLogin = FALSE;
        NET_DVR_DEVICEINFO_V40 info{};
        user_ = NET_DVR_Login_V40(&login, &info);
        if (user_ < 0) {
            const DWORD error = NET_DVR_GetLastError();
            // HCNetSDK 错误 7 表示设备连接失败；前端需要按网络不可达展示，而不是误导为账号错误。
            if (error == 7) throw BridgeError(502, "NVR_UNREACHABLE", "无法连接 NVR " + request_.ip + ":" + std::to_string(request_.port) + " 的 SDK 端口；请检查设备 IP、SDK 端口、网络连通性或防火墙。");
            throw BridgeError(502, "SDK_LOGIN_FAILED", "NVR 登录失败：HCNetSDK 错误=" + std::to_string(error) + "。请检查用户名、密码、设备状态和登录权限。");
        }
        const auto login_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - login_started).count();
        log_line("INFO", "sid=" + request_.sid + " NVR 登录成功：耗时=" + std::to_string(login_elapsed) + "ms。");
        topology_ = NvrChannelTopology::read(user_, info, request_.sid);
        sdk_camera_ = topology_.resolve(request_.camera, request_.channel_type, resolved_channel_type_);
        log_line("INFO", "sid=" + request_.sid + " SDK 通道已解析：请求通道=" + std::to_string(request_.camera) + "，请求类型=" + channel_type_name(request_.channel_type) + "，实际通道=" + std::to_string(sdk_camera_) + "，实际类型=" + resolved_channel_type_name(resolved_channel_type_) + "。");
        read_video_stream_configuration();
        prepared_ = true;
    }

    void start_stream_and_wait_for_data() {
        prepare();
        if (!stream_started_) {
            if (request_.option == Option::RealPlay) start_realplay(); else start_playback();
            stream_started_ = true;
            log_line("INFO", "sid=" + request_.sid + " NVR 通道建流已提交：模式=" + std::string(request_.option == Option::RealPlay ? "实时预览" : "视频回放") + "，请求通道=" + std::to_string(request_.camera) + "，请求类型=" + channel_type_name(request_.channel_type) + "，实际通道=" + std::to_string(sdk_camera_) + "，实际类型=" + resolved_channel_type_name(resolved_channel_type_) + "，码流=" + (request_.stream == Stream::Main ? "主码流" : "子码流") + "，速度=" + std::to_string(request_.speed) + "x。");
        }
        std::unique_lock<std::mutex> lock(first_mutex_);
        if (!first_cv_.wait_for(lock, std::chrono::milliseconds(config_.sdk_start_ms), [&] { return first_data_ || overflow_; })) {
            throw BridgeError(504, "SDK_STREAM_TIMEOUT", "waiting for HCNetSDK stream timed out");
        }
        if (overflow_) throw BridgeError(502, "MEDIA_QUEUE_OVERFLOW", "HCNetSDK media queue backpressure limit exceeded");
        log_line("INFO", "sid=" + request_.sid + " HCNetSDK 已获取首个视频数据。");
    }

private:
    void read_video_stream_configuration() {
        if (!config_.read_video_codec_from_device && (request_.option != Option::RealPlay || config_.realplay_keyframe_interval_frames <= 0)) return;
        NET_DVR_COMPRESSIONCFG_V30 compression{};
        compression.dwSize = sizeof(compression);
        DWORD returned = 0;
        if (!NET_DVR_GetDVRConfig(user_, NET_DVR_GET_COMPRESSCFG_V30, sdk_camera_, &compression, sizeof(compression), &returned)) {
            log_line("INFO", "sid=" + request_.sid + " 未读取到 NVR 码流编码参数，将按实际视频数据探测：HCNetSDK 错误=" + std::to_string(NET_DVR_GetLastError()) + "。");
            return;
        }
        auto* selected = request_.stream == Stream::Main ? &compression.struNormHighRecordPara : &compression.struNetPara;
        if (config_.read_video_codec_from_device) {
            configured_video_codec_ = selected->byVideoEncType == 10 ? DetectedVideoCodec::H265 :
                (selected->byVideoEncType == 0 || selected->byVideoEncType == 1 ? DetectedVideoCodec::H264 : DetectedVideoCodec::Unknown);
            if (configured_video_codec_ != DetectedVideoCodec::Unknown)
                log_line("INFO", "sid=" + request_.sid + " 已从 NVR 读取码流编码：码流=" + std::string(request_.stream == Stream::Main ? "主码流" : "子码流") + "，编码=" + video_codec_name(configured_video_codec_) + "。");
            else
                log_line("INFO", "sid=" + request_.sid + " NVR 未返回可识别的视频编码类型，将按实际视频数据探测。");
        }
        if (request_.option != Option::RealPlay || config_.realplay_keyframe_interval_frames <= 0) return;
        const auto requested = static_cast<WORD>(config_.realplay_keyframe_interval_frames);
        if (selected->wIntervalFrameI != 0 && selected->wIntervalFrameI != 0xfffe && selected->wIntervalFrameI <= requested) return;
        selected->wIntervalFrameI = requested;
        if (NET_DVR_SetDVRConfig(user_, NET_DVR_SET_COMPRESSCFG_V30, sdk_camera_, &compression, sizeof(compression)))
            log_line("INFO", "sid=" + request_.sid + " 已将 NVR " + std::string(request_.stream == Stream::Main ? "主码流" : "子码流") + " I 帧间隔缩短为 " + std::to_string(requested) + " 帧。");
        else
            log_line("WARN", "sid=" + request_.sid + " 无法修改 NVR I 帧间隔，将继续使用设备当前设置：HCNetSDK 错误=" + std::to_string(NET_DVR_GetLastError()) + "。");
    }
    static void CALLBACK real_callback(LONG, DWORD type, BYTE* data, DWORD size, void* user) {
        static_cast<HcSession*>(user)->accept_data(type, data, size);
    }
    static void CALLBACK playback_callback(LONG, DWORD type, BYTE* data, DWORD size, void* user) {
        static_cast<HcSession*>(user)->accept_data(type, data, size);
    }
    void accept_data(DWORD type, BYTE* data, DWORD size) {
        if (stopped_.load() || !data || size == 0 || (type != NET_DVR_SYSHEAD && type != NET_DVR_STREAMDATA && type != NET_DVR_STD_VIDEODATA)) return;
        if (end_reason() != StreamEndReason::Active) return;
        sdk_bytes_.fetch_add(size);
        if (type == NET_DVR_SYSHEAD) ++header_packets_;
        else ++media_packets_;
        std::vector<uint8_t> packet(data, data + size);
        if (!queue_.push(std::move(packet), std::chrono::milliseconds(config_.queue_backpressure_ms))) {
            overflow_ = true;
            end_reason_.store(StreamEndReason::QueueOverflow);
            log_line("WARN", "sid=" + request_.sid + " HCNetSDK 媒体队列背压超限：当前/峰值=" + std::to_string(queue_.current_bytes()) + "/" + std::to_string(queue_.peak_bytes()) + " 字节，容量=" + std::to_string(config_.queue_bytes) + " 字节，背压次数=" + std::to_string(queue_.backpressure_events()) + "；结束会话以便前端恢复。");
            queue_.close();
            first_cv_.notify_all();
            return;
        }
        if (type != NET_DVR_SYSHEAD) {
            std::lock_guard<std::mutex> lock(first_mutex_);
            first_data_ = true;
            first_cv_.notify_all();
        }
    }
    void start_realplay() {
        NET_DVR_PREVIEWINFO preview{};
        preview.lChannel = sdk_camera_;
        preview.dwStreamType = request_.stream == Stream::Main ? 0 : 1;
        preview.dwLinkMode = 0;
        preview.hPlayWnd = 0;
        preview.bBlocked = 1;
        preview.dwDisplayBufNum = 1;
        handle_ = NET_DVR_RealPlay_V40(user_, &preview, real_callback, this);
        if (handle_ < 0) sdk_error("SDK_REALPLAY_FAILED", "NET_DVR_RealPlay_V40");
        log_line("INFO", "sid=" + request_.sid + " 已启动实时取流：通道=" + std::to_string(sdk_camera_) + "，码流=" + (request_.stream == Stream::Main ? "主码流" : "子码流") + "，传输=TCP。");
        request_realplay_keyframe();
    }
    void request_realplay_keyframe() {
        if (!config_.force_keyframe_on_realplay) return;
        const BOOL success = request_.stream == Stream::Main ? NET_DVR_MakeKeyFrame(user_, sdk_camera_) : NET_DVR_MakeKeyFrameSub(user_, sdk_camera_);
        if (success)
            log_line("INFO", "sid=" + request_.sid + " 已请求 NVR 立即发送 " + std::string(request_.stream == Stream::Main ? "主码流" : "子码流") + "关键帧。");
        else
            log_line("INFO", "sid=" + request_.sid + " NVR 不支持或拒绝立即发送关键帧，将等待设备自然关键帧：HCNetSDK 错误=" + std::to_string(NET_DVR_GetLastError()) + "。");
    }
    void start_playback() {
        NET_DVR_VOD_PARA vod{};
        vod.dwSize = sizeof(vod);
        vod.struIDInfo.dwSize = sizeof(vod.struIDInfo);
        vod.struIDInfo.dwChannel = sdk_camera_;
        vod.struBeginTime = unix_to_nvr_time(request_.start);
        vod.struEndTime = unix_to_nvr_time(request_.end);
        vod.hWnd = 0;
        vod.byStreamType = request_.stream == Stream::Main ? 0 : 1;
        handle_ = NET_DVR_PlayBackByTime_V40(user_, &vod);
        if (handle_ < 0) sdk_error("SDK_PLAYBACK_FAILED", "NET_DVR_PlayBackByTime_V40");
        if (!NET_DVR_SetPlayDataCallBack_V40(handle_, playback_callback, this)) sdk_error("SDK_CALLBACK_FAILED", "NET_DVR_SetPlayDataCallBack_V40");
        control(NET_DVR_PLAYSTART);
        if (request_.speed != 1) {
            const DWORD command = request_.speed > 1 ? NET_DVR_PLAYFAST : NET_DVR_PLAYSLOW;
            const int steps = static_cast<int>(std::llround(std::abs(std::log2(request_.speed))));
            for (int i = 0; i < steps; ++i) control(command);
        }
        log_line("INFO", "sid=" + request_.sid + " 已启动回放取流：通道=" + std::to_string(sdk_camera_) + "，开始=" + std::to_string(request_.start) + "，结束=" + std::to_string(request_.end) + "，速度=" + std::to_string(request_.speed) + "x。");
        monitor_ = std::thread([this] {
            auto next_position_check = std::chrono::steady_clock::now();
            auto next_keep_alive = next_position_check + std::chrono::milliseconds(config_.playback_keep_alive_ms);
            while (!stopped_.load()) {
                std::this_thread::sleep_for(200ms);
                const auto now = std::chrono::steady_clock::now();
                if (now >= next_keep_alive) {
                    send_playback_keep_alive();
                    next_keep_alive = now + std::chrono::milliseconds(config_.playback_keep_alive_ms);
                    if (end_reason() != StreamEndReason::Active) return;
                }
                if (now < next_position_check) continue;
                next_position_check = now + 500ms;
                const DWORD position = NET_DVR_GetPlayBackPos(handle_);
                if (position == 100) { complete_playback(StreamEndReason::PlaybackEnd, "回放已到达结束位置。", false); return; }
                if (position == 200) { complete_playback(StreamEndReason::PlaybackPositionError, "回放位置状态异常。", true); return; }
            }
        });
    }
    void send_playback_keep_alive() {
        if (NET_DVR_PlayBackControl_V40(handle_, NET_DVR_KEEPALIVE, nullptr, 0, nullptr, nullptr)) {
            if (keep_alive_failures_ > 0) log_line("INFO", "sid=" + request_.sid + " 回放保活已恢复：此前连续失败=" + std::to_string(keep_alive_failures_) + " 次。");
            keep_alive_failures_ = 0;
            return;
        }
        const int failures = ++keep_alive_failures_;
        log_line("WARN", "sid=" + request_.sid + " 回放保活失败：连续失败=" + std::to_string(failures) + " 次，HCNetSDK 错误=" + std::to_string(NET_DVR_GetLastError()) + "。");
        // 单次失败允许设备和网络自行恢复；连续失败才由前端按实际播放位置重建会话。
        if (failures >= 3) complete_playback(StreamEndReason::SdkKeepAliveFailed, "回放保活连续失败，结束当前会话以便前端恢复。", true);
    }
    void complete_playback(StreamEndReason reason, const std::string& message, bool warning) {
        if (end_reason() != StreamEndReason::Active) return;
        end_reason_.store(reason);
        log_line(warning ? "WARN" : "INFO", "sid=" + request_.sid + " " + message);
        queue_.close();
    }
    void control(DWORD command) {
        if (!NET_DVR_PlayBackControl_V40(handle_, command, nullptr, 0, nullptr, nullptr)) sdk_error("SDK_PLAYBACK_CONTROL_FAILED", "NET_DVR_PlayBackControl_V40");
    }
    [[noreturn]] void sdk_error(const std::string& code, const std::string& api) const {
        throw BridgeError(502, code, api + " failed, HCNetSDK error=" + std::to_string(NET_DVR_GetLastError()));
    }
    void stop() {
        if (stopped_.exchange(true)) return;
        queue_.close();
        if (monitor_.joinable()) monitor_.join();
        if (handle_ >= 0) {
            if (request_.option == Option::RealPlay) NET_DVR_StopRealPlay(handle_); else NET_DVR_StopPlayBack(handle_);
            handle_ = -1;
        }
        if (user_ >= 0) { NET_DVR_Logout(user_); user_ = -1; }
        log_line("INFO", "sid=" + request_.sid + " HCNetSDK 会话已释放：原因=" + std::string(end_reason_name(end_reason())) + "，媒体包=" + std::to_string(media_packets_.load()) + "，数据=" + std::to_string(sdk_bytes_.load()) + " 字节，队列峰值=" + std::to_string(queue_.peak_bytes()) + " 字节，背压次数=" + std::to_string(queue_.backpressure_events()) + "。");
    }
    const StreamRequest& request_;
    const Config& config_;
    ByteQueue queue_;
    LONG user_{-1};
    LONG handle_{-1};
    std::atomic_bool stopped_{false};
    std::atomic_bool overflow_{false};
    std::atomic<StreamEndReason> end_reason_{StreamEndReason::Active};
    std::atomic<unsigned long long> sdk_bytes_{0};
    std::atomic<unsigned long long> header_packets_{0};
    std::atomic<unsigned long long> media_packets_{0};
    NvrChannelTopology topology_{};
    ResolvedChannelType resolved_channel_type_{ResolvedChannelType::Unknown};
    int sdk_camera_{0};
    int keep_alive_failures_{0};
    bool prepared_{false};
    bool stream_started_{false};
    DetectedVideoCodec configured_video_codec_{DetectedVideoCodec::Unknown};
    bool first_data_{false};
    std::mutex first_mutex_;
    std::condition_variable first_cv_;
    std::thread monitor_;
};

bool write_all(int fd, const uint8_t* data, size_t size) {
    while (size > 0) {
        const ssize_t sent = ::write(fd, data, size);
        if (sent > 0) { data += sent; size -= static_cast<size_t>(sent); continue; }
        if (sent < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

enum class VideoOutputMode { Copy, H264Transcode };
const char* video_codec_name(DetectedVideoCodec codec) {
    switch (codec) {
        case DetectedVideoCodec::H264: return "H.264";
        case DetectedVideoCodec::H265: return "H.265";
        default: return "未识别";
    }
}

// 缓存按设备、真实/索引通道及主/子码流隔离，且刻意不把用户名、密码写入键值或日志。
// 状态文件只保存编码和过期时间；服务重启后的已知通道无需再次进行实际码流探测。
class VideoCodecCache {
public:
    VideoCodecCache(int ttl_seconds, fs::path cache_file) : ttl_(ttl_seconds), cache_file_(std::move(cache_file)) { load(); }

    bool try_get(const StreamRequest& request, DetectedVideoCodec& codec) {
        codec = DetectedVideoCodec::Unknown;
        if (ttl_ <= 0) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = make_key(request);
        const auto item = items_.find(key);
        if (item == items_.end()) return false;
        if (item->second.expires <= std::time(nullptr)) { items_.erase(item); persist_locked(); return false; }
        codec = item->second.codec;
        return codec == DetectedVideoCodec::H264 || codec == DetectedVideoCodec::H265;
    }

    void set(const StreamRequest& request, DetectedVideoCodec codec) {
        if (ttl_ <= 0 || (codec != DetectedVideoCodec::H264 && codec != DetectedVideoCodec::H265)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        items_[make_key(request)] = Entry{codec, std::time(nullptr) + ttl_};
        persist_locked();
    }

private:
    struct Entry { DetectedVideoCodec codec; std::time_t expires; };
    static std::string make_key(const StreamRequest& request) {
        return request.ip + "\n" + std::to_string(request.port) + "\n" + std::to_string(request.camera) + "\n" +
            std::to_string(static_cast<int>(request.channel_type)) + "\n" + std::to_string(static_cast<int>(request.stream));
    }
    static std::string hex_encode(const std::string& input) {
        std::ostringstream value;
        value << std::hex << std::setfill('0');
        for (const unsigned char ch : input) value << std::setw(2) << static_cast<unsigned>(ch);
        return value.str();
    }
    static std::optional<std::string> hex_decode(const std::string& input) {
        if (input.size() % 2 != 0) return std::nullopt;
        std::string result;
        result.reserve(input.size() / 2);
        for (size_t index = 0; index < input.size(); index += 2) {
            if (!std::isxdigit(static_cast<unsigned char>(input[index])) || !std::isxdigit(static_cast<unsigned char>(input[index + 1]))) return std::nullopt;
            result.push_back(static_cast<char>(std::strtol(input.substr(index, 2).c_str(), nullptr, 16)));
        }
        return result;
    }
    void load() {
        if (ttl_ <= 0 || cache_file_.empty()) return;
        std::ifstream input(cache_file_);
        if (!input) return;
        const auto now = std::time(nullptr);
        std::string line;
        int count = 0;
        while (std::getline(input, line)) {
            std::istringstream row(line);
            std::string expires_text, codec_text, encoded_key;
            if (!std::getline(row, expires_text, '\t') || !std::getline(row, codec_text, '\t') || !std::getline(row, encoded_key)) continue;
            std::time_t expires{};
            try { expires = static_cast<std::time_t>(std::stoll(expires_text)); } catch (...) { continue; }
            const auto key = hex_decode(encoded_key);
            const auto codec = codec_text == "h264" ? DetectedVideoCodec::H264 : codec_text == "h265" ? DetectedVideoCodec::H265 : DetectedVideoCodec::Unknown;
            if (!key || expires <= now || codec == DetectedVideoCodec::Unknown) continue;
            items_[*key] = Entry{codec, expires};
            ++count;
        }
        if (count > 0) log_line("INFO", "已加载持久化视频编码缓存：条目=" + std::to_string(count) + "。");
    }
    void persist_locked() {
        if (ttl_ <= 0 || cache_file_.empty()) return;
        try {
            std::error_code error;
            fs::create_directories(cache_file_.parent_path(), error);
            const auto temporary = cache_file_.string() + ".tmp";
            std::ofstream output(temporary, std::ios::out | std::ios::trunc);
            if (!output) throw std::runtime_error("open failed");
            const auto now = std::time(nullptr);
            for (const auto& item : items_) {
                if (item.second.expires <= now) continue;
                output << item.second.expires << '\t' << (item.second.codec == DetectedVideoCodec::H264 ? "h264" : "h265") << '\t' << hex_encode(item.first) << '\n';
            }
            output.close();
            fs::remove(cache_file_, error);
            fs::rename(temporary, cache_file_, error);
            if (error) throw std::runtime_error(error.message());
        } catch (...) {
            log_line("WARN", "无法保存持久化视频编码缓存，将继续使用内存缓存。");
        }
    }
    int ttl_;
    fs::path cache_file_;
    std::mutex mutex_;
    std::map<std::string, Entry> items_;
};

class FfmpegProcess {
public:
    FfmpegProcess(const Config& config, const StreamRequest& request, VideoOutputMode video_output_mode)
        : config_(config), sid_(request.sid), video_output_mode_(video_output_mode) {
        int input_pipe[2]{};
        int output_pipe[2]{};
        int error_pipe[2]{};
        if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0 || ::pipe(error_pipe) != 0) throw BridgeError(500, "PIPE_FAILED", "cannot create ffmpeg pipe");
        pid_ = ::fork();
        if (pid_ < 0) throw BridgeError(500, "FFMPEG_START_FAILED", "cannot fork ffmpeg");
        if (pid_ == 0) {
            ::dup2(input_pipe[0], STDIN_FILENO);
            ::dup2(output_pipe[1], STDOUT_FILENO);
            ::dup2(error_pipe[1], STDERR_FILENO);
            ::close(input_pipe[0]); ::close(input_pipe[1]); ::close(output_pipe[0]); ::close(output_pipe[1]); ::close(error_pipe[0]); ::close(error_pipe[1]);
            // 设备码流经 HCNetSDK 管道输入；缩小探测窗口可更快生成 fMP4 初始化段。
            std::vector<std::string> args{"ffmpeg", "-hide_banner", "-loglevel", config.ffmpeg_log_level, "-fflags", "+genpts+nobuffer", "-analyzeduration", "250000", "-probesize", "262144", "-i", "pipe:0", "-map", "0:v:0"};
            if (video_output_mode == VideoOutputMode::Copy) args.insert(args.end(), {"-c:v", "copy"});
            else {
                args.insert(args.end(), {"-c:v", config.video_encoder, "-preset", config.video_preset, "-tune", "zerolatency", "-pix_fmt", "yuv420p", "-profile:v", "high", "-level:v", "4.1", "-g", "25", "-keyint_min", "25", "-sc_threshold", "0"});
                // 浏览器按 fMP4 PTS 播放；HCNetSDK 仅改变帧到达速度不足以实现变速，必须在此重写时间戳。
                if (request.speed != 1) {
                    std::ostringstream speed;
                    speed << std::setprecision(8) << request.speed;
                    args.insert(args.end(), {"-vf", "setpts=PTS/" + speed.str()});
                }
            }
            if (config.enable_audio && request.speed == 1) args.insert(args.end(), {"-map", "0:a:0?", "-c:a", "aac", "-ar", "48000", "-ac", "2"});
            else args.insert(args.end(), {"-an"});
            // 250ms 分片可让 MSE 在初始化段之后更早拿到首个媒体片段。
            args.insert(args.end(), {"-f", "mp4", "-movflags", "frag_keyframe+empty_moov+default_base_moof+omit_tfhd_offset", "-frag_duration", "250000", "pipe:1"});
            std::vector<char*> argv;
            for (auto& item : args) argv.push_back(item.data());
            argv.push_back(nullptr);
            ::execv(config.ffmpeg_path.c_str(), argv.data());
            _exit(127);
        }
        ::close(input_pipe[0]); ::close(output_pipe[1]); ::close(error_pipe[1]);
        input_fd_ = input_pipe[1];
        output_fd_ = output_pipe[0];
        error_fd_ = error_pipe[0];
        error_pump_ = std::thread([this] { read_diagnostics(); });
        log_line("INFO", "sid=" + sid_ + " FFmpeg 转流进程已启动：模式=" + std::string(video_output_mode == VideoOutputMode::Copy ? "H.264 直通/编码探测" : "转码为 H.264") + "，速度=" + std::to_string(request.speed) + "x，音频=" + std::string(config.enable_audio && request.speed == 1 ? "启用" : "关闭") + "。");
    }
    ~FfmpegProcess() {
        stop_requested_.store(true);
        if (input_fd_ >= 0) ::close(input_fd_);
        if (output_fd_ >= 0) ::close(output_fd_);
        if (pid_ > 0) { ::kill(pid_, SIGTERM); int status = 0; ::waitpid(pid_, &status, 0); }
        if (error_fd_ >= 0) ::close(error_fd_);
        if (pump_.joinable()) pump_.join();
        if (error_pump_.joinable()) error_pump_.join();
    }
    // H.265 回退探测结束后立即结束探测 FFmpeg，但不关闭共享的 HCNetSDK 队列；
    // 新的 H.264 转码进程可继续消费同一 NVR 取流，避免第二次登录和等待关键帧。
    void stop_for_pipeline_switch() {
        stop_requested_.store(true);
        if (input_fd_ >= 0) { ::close(input_fd_); input_fd_ = -1; }
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status = 0;
            ::waitpid(pid_, &status, 0);
            pid_ = -1;
        }
        if (pump_.joinable()) pump_.join();
        if (output_fd_ >= 0) { ::close(output_fd_); output_fd_ = -1; }
        if (error_fd_ >= 0) { ::close(error_fd_); error_fd_ = -1; }
        if (error_pump_.joinable()) error_pump_.join();
    }
    void start_pump(ByteQueue& queue) {
        pump_ = std::thread([this, &queue] {
            std::vector<uint8_t> data;
            auto last_data = std::chrono::steady_clock::now();
            while (!stop_requested_.load()) {
                if (!queue.pop(data, 200ms)) {
                    if (stop_requested_.load() || queue.closed()) break;
                    if (std::chrono::steady_clock::now() - last_data < std::chrono::milliseconds(config_.no_sdk_data_ms)) continue;
                    input_end_reason_.store(StreamEndReason::SdkDataTimeout);
                    break;
                }
                last_data = std::chrono::steady_clock::now();
                if (!write_all(input_fd_, data.data(), data.size())) break;
            }
            if (!stop_requested_.load() && !queue.closed() && input_end_reason() != StreamEndReason::SdkDataTimeout) input_end_reason_.store(StreamEndReason::SdkDataTimeout);
            if (!stop_requested_.load() && input_end_reason() == StreamEndReason::SdkDataTimeout)
                log_line("WARN", "sid=" + sid_ + " FFmpeg 输入超时：HCNetSDK 在 " + std::to_string(config_.no_sdk_data_ms) + "ms 内未提供数据。");
            if (input_fd_ >= 0) { ::close(input_fd_); input_fd_ = -1; }
        });
    }
    std::vector<uint8_t> read_initial(int timeout_ms) {
        std::vector<uint8_t> result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        uint8_t buffer[32768];
        while (std::chrono::steady_clock::now() < deadline && result.size() < 4 * 1024 * 1024) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            pollfd poll_fd{output_fd_, POLLIN, 0};
            if (::poll(&poll_fd, 1, static_cast<int>(std::max<long long>(1, remaining))) <= 0) continue;
            const ssize_t read_size = ::read(output_fd_, buffer, sizeof(buffer));
            if (read_size <= 0) break;
            result.insert(result.end(), buffer, buffer + read_size);
            if (contains_atom(result, "ftyp") && contains_atom(result, "moov")) return result;
        }
        throw BridgeError(502, "FFMPEG_NO_MP4", "FFmpeg did not produce a fragmented MP4 header");
    }
    ssize_t read(uint8_t* buffer, size_t size) { return ::read(output_fd_, buffer, size); }
    StreamEndReason input_end_reason() const { return input_end_reason_.load(); }
private:
    void read_diagnostics() {
        std::string pending;
        char buffer[1024];
        while (error_fd_ >= 0) {
            const ssize_t count = ::read(error_fd_, buffer, sizeof(buffer));
            if (count <= 0) break;
            pending.append(buffer, static_cast<size_t>(count));
            size_t end = 0;
            while ((end = pending.find_first_of("\r\n")) != std::string::npos) {
                const auto line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (line.find("Video: hevc") != std::string::npos || line.find("Video: h265") != std::string::npos)
                    log_line("INFO", "sid=" + sid_ + " FFmpeg 已识别输入视频编码=H.265，处理方式=" + std::string(video_output_mode_ == VideoOutputMode::Copy ? "编码探测" : "转码为 H.264") + "。");
                else if (line.find("Video: h264") != std::string::npos || line.find("Video: avc") != std::string::npos)
                    log_line("INFO", "sid=" + sid_ + " FFmpeg 已识别输入视频编码=H.264，处理方式=" + std::string(video_output_mode_ == VideoOutputMode::Copy ? "H.264 直通" : "转码为 H.264") + "。");
            }
        }
    }
    static bool contains_atom(const std::vector<uint8_t>& data, const char* atom) {
        return std::search(data.begin(), data.end(), atom, atom + 4) != data.end();
    }
    const Config& config_;
    std::string sid_;
    VideoOutputMode video_output_mode_;
    pid_t pid_{-1};
    int input_fd_{-1};
    int output_fd_{-1};
    int error_fd_{-1};
    std::thread pump_;
    std::thread error_pump_;
    std::atomic_bool stop_requested_{false};
    std::atomic<StreamEndReason> input_end_reason_{StreamEndReason::Active};
};

class SessionRegistry {
public:
    explicit SessionRegistry(int max) : max_(max) {}
    class Lease {
    public:
        explicit Lease(SessionRegistry* owner) : owner_(owner) {}
        Lease(const Lease&) = delete;
        ~Lease() { if (owner_) owner_->release(); }
    private: SessionRegistry* owner_;
    };
    std::unique_ptr<Lease> reserve() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ >= max_) return {};
        ++active_;
        return std::make_unique<Lease>(this);
    }
    int active() const { std::lock_guard<std::mutex> lock(mutex_); return active_; }
private:
    void release() { std::lock_guard<std::mutex> lock(mutex_); --active_; }
    const int max_;
    mutable std::mutex mutex_;
    int active_{0};
};

struct SessionStatus {
    std::string sid;
    std::string status{"active"};
    std::string end_reason{"active"};
    std::string message;
    size_t queue_current_bytes{0};
    size_t queue_peak_bytes{0};
    size_t backpressure_events{0};
    std::chrono::steady_clock::time_point updated{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    std::optional<std::chrono::steady_clock::time_point> output_ready;
    std::optional<std::chrono::steady_clock::time_point> browser_first_frame;
    std::optional<std::time_t> output_ready_utc;
    std::optional<std::time_t> browser_first_frame_utc;
    long long browser_client_elapsed_ms{0};
    std::time_t updated_utc{std::time(nullptr)};
};

// HTTP 响应已经开始后无法返回 JSON 错误；保留短期状态让 MSE 客户端在 EOF 后判断是否重连。
class SessionStatusRegistry {
public:
    void set_active(const std::string& sid) {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanup_locked();
        items_[sid] = SessionStatus{sid};
    }
    void set_ended(const std::string& sid, StreamEndReason reason, const std::string& message, size_t current, size_t peak, size_t backpressure) {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanup_locked();
        auto existing = items_.find(sid);
        SessionStatus value = existing == items_.end() ? SessionStatus{} : existing->second;
        value.sid = sid;
        value.status = "ended";
        value.end_reason = end_reason_name(reason);
        value.message = message;
        value.queue_current_bytes = current;
        value.queue_peak_bytes = peak;
        value.backpressure_events = backpressure;
        value.updated = std::chrono::steady_clock::now();
        value.updated_utc = std::time(nullptr);
        items_[sid] = std::move(value);
    }
    void set_output_ready(const std::string& sid) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto item = items_.find(sid);
        if (item == items_.end()) return;
        const auto now = std::chrono::steady_clock::now();
        item->second.output_ready = now;
        item->second.updated = now;
        item->second.output_ready_utc = std::time(nullptr);
        item->second.updated_utc = *item->second.output_ready_utc;
    }
    std::optional<std::pair<long long, long long>> set_browser_first_frame(const std::string& sid, long long client_elapsed_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto item = items_.find(sid);
        if (item == items_.end()) return std::nullopt;
        const auto now = std::chrono::steady_clock::now();
        if (!item->second.browser_first_frame) {
            item->second.browser_first_frame = now;
            item->second.browser_first_frame_utc = std::time(nullptr);
        }
        item->second.browser_client_elapsed_ms = client_elapsed_ms;
        item->second.updated = now;
        item->second.updated_utc = std::time(nullptr);
        const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(*item->second.browser_first_frame - item->second.started).count();
        const auto output_to_frame = item->second.output_ready ? std::chrono::duration_cast<std::chrono::milliseconds>(*item->second.browser_first_frame - *item->second.output_ready).count() : -1;
        return std::make_pair(total, output_to_frame);
    }
    std::optional<SessionStatus> get(const std::string& sid) {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanup_locked();
        const auto item = items_.find(sid);
        return item == items_.end() ? std::nullopt : std::optional<SessionStatus>(item->second);
    }
private:
    void cleanup_locked() {
        const auto threshold = std::chrono::steady_clock::now() - std::chrono::minutes(10);
        for (auto item = items_.begin(); item != items_.end();) {
            if (item->second.updated < threshold) item = items_.erase(item); else ++item;
        }
    }
    std::mutex mutex_;
    std::map<std::string, SessionStatus> items_;
};

struct HttpRequest { std::string method; std::string path; Query query; };

HttpRequest read_http_request(int fd) {
    std::string raw;
    char buffer[4096];
    while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < 16384) {
        const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0) throw ClientDisconnected();
        raw.append(buffer, static_cast<size_t>(count));
    }
    const auto line_end = raw.find("\r\n");
    if (line_end == std::string::npos) throw BridgeError(400, "BAD_REQUEST", "invalid HTTP request");
    std::istringstream line(raw.substr(0, line_end));
    std::string target, version;
    HttpRequest request;
    if (!(line >> request.method >> target >> version)) throw BridgeError(400, "BAD_REQUEST", "invalid HTTP request line");
    const auto query_at = target.find('?');
    request.path = target.substr(0, query_at);
    request.query = query_at == std::string::npos ? Query{} : parse_query(target.substr(query_at + 1));
    return request;
}

void send_text(int fd, const std::string& text) {
    if (!write_all(fd, reinterpret_cast<const uint8_t*>(text.data()), text.size())) throw ClientDisconnected();
}

std::string json_escape(const std::string& value) {
    std::string result;
    for (const auto c : value) { if (c == '"' || c == '\\') result.push_back('\\'); result.push_back(c); }
    return result;
}

void send_json_body(int fd, int status, const std::string& body) {
    const std::string status_text = status == 200 ? "OK" : status == 204 ? "No Content" : status == 400 ? "Bad Request" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : status == 503 ? "Service Unavailable" : "Bad Gateway";
    send_text(fd, "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) + "\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n" + body);
}

void send_json(int fd, int status, const std::string& code, const std::string& message, const std::string& sid = "") {
    const std::string body = "{\"code\":\"" + json_escape(code) + "\",\"message\":\"" + json_escape(message) + "\"" + (sid.empty() ? "" : ",\"requestId\":\"" + json_escape(sid) + "\"") + "}";
    send_json_body(fd, status, body);
}

std::string format_utc(std::time_t value) {
    std::tm utc{};
    gmtime_r(&value, &utc);
    std::ostringstream text;
    text << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return text.str();
}

void send_session_status(int fd, const SessionStatus& value) {
    const std::string output_ready = value.output_ready_utc ? "\"" + format_utc(*value.output_ready_utc) + "\"" : "null";
    const std::string browser_first_frame = value.browser_first_frame_utc ? "\"" + format_utc(*value.browser_first_frame_utc) + "\"" : "null";
    const std::string body = "{\"sid\":\"" + json_escape(value.sid) + "\",\"status\":\"" + json_escape(value.status) + "\",\"endReason\":\"" + json_escape(value.end_reason) + "\",\"message\":\"" + json_escape(value.message) + "\",\"queueCurrentBytes\":" + std::to_string(value.queue_current_bytes) + ",\"queuePeakBytes\":" + std::to_string(value.queue_peak_bytes) + ",\"backpressureEvents\":" + std::to_string(value.backpressure_events) + ",\"outputReadyUtc\":" + output_ready + ",\"browserFirstFrameUtc\":" + browser_first_frame + ",\"updatedUtc\":\"" + format_utc(value.updated_utc) + "\"}";
    send_text(fd, "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n" + body);
}

bool initial_mp4_has_audio(const std::vector<uint8_t>& initial_mp4) {
    const std::array<uint8_t, 4> atom{{'s', 'o', 'u', 'n'}};
    return std::search(initial_mp4.begin(), initial_mp4.end(), atom.begin(), atom.end()) != initial_mp4.end();
}

size_t find_atom(const std::vector<uint8_t>& data, const char* atom) {
    const auto position = std::search(data.begin(), data.end(), atom, atom + 4);
    return position == data.end() ? data.size() : static_cast<size_t>(position - data.begin());
}

DetectedVideoCodec detect_video_codec(const std::vector<uint8_t>& initial_mp4) {
    if (find_atom(initial_mp4, "hvcC") != initial_mp4.size()) return DetectedVideoCodec::H265;
    if (find_atom(initial_mp4, "avcC") != initial_mp4.size()) return DetectedVideoCodec::H264;
    return DetectedVideoCodec::Unknown;
}

std::string avc_mse_codec(const std::vector<uint8_t>& initial_mp4) {
    const auto at = find_atom(initial_mp4, "avcC");
    if (at == initial_mp4.size() || at + 7 >= initial_mp4.size()) return "avc1.640029";
    std::ostringstream value;
    value << "avc1." << std::hex << std::nouppercase << std::setfill('0')
          << std::setw(2) << static_cast<unsigned>(initial_mp4[at + 5])
          << std::setw(2) << static_cast<unsigned>(initial_mp4[at + 6])
          << std::setw(2) << static_cast<unsigned>(initial_mp4[at + 7]);
    return value.str();
}

void send_video_headers(int fd, const StreamRequest& request, const Config& config, const std::vector<uint8_t>& initial_mp4) {
    // 初始化段含实际轨道；仅在存在音频轨时声明 mp4a，避免 MSE codec 列表与设备流不一致。
    const std::string video_codec = avc_mse_codec(initial_mp4);
    const std::string codecs = config.enable_audio && request.speed == 1 && initial_mp4_has_audio(initial_mp4) ? video_codec + ",mp4a.40.2" : video_codec;
    const std::string playback_start = request.option == Option::Playback ? std::to_string(request.start) : "";
    const std::string playback_end = request.option == Option::Playback ? std::to_string(request.end) : "";
    send_text(fd, "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nTransfer-Encoding: chunked\r\nCache-Control: no-store, no-cache\r\nAccept-Ranges: none\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: X-Hik-Bridge-Mse-Codecs, X-Hik-Bridge-Video-Codec, X-Hik-Bridge-Playback-Start, X-Hik-Bridge-Playback-End, X-Hik-Bridge-Session-Id, X-Hik-Bridge-Session-Status-Url\r\nX-Hik-Bridge-Mse-Codecs: " + codecs + "\r\nX-Hik-Bridge-Video-Codec: h264\r\nX-Hik-Bridge-Playback-Start: " + playback_start + "\r\nX-Hik-Bridge-Playback-End: " + playback_end + "\r\nX-Hik-Bridge-Session-Id: " + request.sid + "\r\nX-Hik-Bridge-Session-Status-Url: /session-status?sid=" + request.sid + "\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n");
}

void send_chunk(int fd, const uint8_t* data, size_t size) {
    std::ostringstream head; head << std::hex << size << "\r\n";
    send_text(fd, head.str());
    if (size && !write_all(fd, data, size)) throw ClientDisconnected();
    send_text(fd, "\r\n");
}

StreamEndReason resolve_end_reason(const HcSession* session, const FfmpegProcess* ffmpeg) {
    if (session && session->end_reason() != StreamEndReason::Active) return session->end_reason();
    if (ffmpeg && ffmpeg->input_end_reason() == StreamEndReason::SdkDataTimeout) return StreamEndReason::SdkDataTimeout;
    if (ffmpeg) return StreamEndReason::FfmpegExit;
    return StreamEndReason::StreamEnded;
}

void handle_video(int fd, const StreamRequest& request, const Config& config, SessionRegistry& registry, SessionStatusRegistry& statuses, VideoCodecCache& codec_cache, bool& response_started) {
    auto lease = registry.reserve();
    if (!lease) throw BridgeError(503, "SESSION_LIMIT", "maximum concurrent session count reached");
    statuses.set_active(request.sid);
    std::unique_ptr<HcSession> session;
    std::unique_ptr<FfmpegProcess> ffmpeg;
    std::unique_ptr<FfmpegProcess> probe_ffmpeg;
    bool status_published = false;
    const auto publish_status = [&](StreamEndReason reason) {
        if (status_published) return;
        statuses.set_ended(request.sid, reason, end_reason_message(reason),
            session ? session->queue_current_bytes() : 0,
            session ? session->queue_peak_bytes() : 0,
            session ? session->queue_backpressure_events() : 0);
        status_published = true;
    };

    try {
        // 先登录、解析真实通道并读取设备编码配置。支持该接口的设备能直接选择正式管线，
        // H.265 不再经历“探测会话释放→重新登录→正式取流”的双会话流程。
        session = std::make_unique<HcSession>(request, config);
        session->prepare();
        DetectedVideoCodec detected_codec = DetectedVideoCodec::Unknown;
        std::vector<uint8_t> prefix;
        const bool cache_hit = codec_cache.try_get(request, detected_codec);
        if (cache_hit) {
            const char* text = video_codec_name(detected_codec);
            log_line("INFO", "sid=" + request.sid + " 命中视频编码缓存：编码=" + text + "。");
        } else if (session->configured_video_codec() != DetectedVideoCodec::Unknown) {
            detected_codec = session->configured_video_codec();
            codec_cache.set(request, detected_codec);
            log_line("INFO", "sid=" + request.sid + " 使用 NVR 编码配置直接建立视频管线，跳过实际码流探测。");
        } else {
            // 仅老设备或 IP 通道不返回压缩参数时探测。探测 FFmpeg 与正式转码将共用 session，
            // 不释放登录、实时句柄和媒体队列。
            log_line("INFO", "sid=" + request.sid + " 开始探测输入视频编码。");
            session->start();
            probe_ffmpeg = std::make_unique<FfmpegProcess>(config, request, VideoOutputMode::Copy);
            probe_ffmpeg->start_pump(session->queue());
            auto probe_prefix = probe_ffmpeg->read_initial(config.first_media_ms);
            detected_codec = detect_video_codec(probe_prefix);
            const char* detected_text = video_codec_name(detected_codec);
            log_line("INFO", "sid=" + request.sid + " 输入视频编码探测完成：编码=" + detected_text + "。");
            codec_cache.set(request, detected_codec);

            if (request.speed == 1 && detected_codec == DetectedVideoCodec::H264) {
                prefix = std::move(probe_prefix);
                ffmpeg = std::move(probe_ffmpeg);
                log_line("INFO", "sid=" + request.sid + " 复用 H.264 编码探测会话，避免重复登录和取流。");
            }
        }

        // 仅 1 倍速 H.264 可零转码。快放/慢放必须以 setpts 重写 fMP4 PTS，因此需要转码。
        auto output_mode = request.speed == 1 && detected_codec == DetectedVideoCodec::H264 ? VideoOutputMode::Copy : VideoOutputMode::H264Transcode;
        log_line("INFO", "sid=" + request.sid + " 编码处理决策：输出=" + std::string(output_mode == VideoOutputMode::Copy ? "H.264 直通" : "转码为 H.264") + "。");
        // H.265/未知编码不能复用 copy 探测进程，但继续复用同一 HCNetSDK 登录、实时句柄和媒体队列。
        if (probe_ffmpeg) {
            probe_ffmpeg->stop_for_pipeline_switch();
            probe_ffmpeg.reset();
            log_line("INFO", "sid=" + request.sid + " 编码探测 FFmpeg 已停止，复用同一 HCNetSDK 取流会话创建正式转码管线。");
        }
        if (!session) {
            session = std::make_unique<HcSession>(request, config);
            session->prepare();
        }
        if (!session->stream_started()) {
            session->start();
        }
        if (!ffmpeg) {
            ffmpeg = std::make_unique<FfmpegProcess>(config, request, output_mode);
            ffmpeg->start_pump(session->queue());
            prefix = ffmpeg->read_initial(config.first_media_ms);
        }
        // 个别旧 NVR 的压缩配置可能与实际 IP 通道码流不一致。以 fMP4 初始化段作最后校验；
        // 若“配置 H.264”实际为 H.265，继续复用当前 HCNetSDK 会话，仅替换 FFmpeg 转码管线。
        if (output_mode == VideoOutputMode::Copy && detect_video_codec(prefix) == DetectedVideoCodec::H265) {
            log_line("WARN", "sid=" + request.sid + " NVR 编码配置与实际码流不一致：配置为 H.264，实际为 H.265；将复用当前 HCNetSDK 会话切换为 H.264 转码。");
            codec_cache.set(request, DetectedVideoCodec::H265);
            ffmpeg->stop_for_pipeline_switch();
            ffmpeg.reset();
            output_mode = VideoOutputMode::H264Transcode;
            ffmpeg = std::make_unique<FfmpegProcess>(config, request, output_mode);
            ffmpeg->start_pump(session->queue());
            prefix = ffmpeg->read_initial(config.first_media_ms);
        }
        log_line("INFO", "sid=" + request.sid + " 视频输出已就绪，开始向浏览器传输：初始化数据=" + std::to_string(prefix.size()) + " 字节。");
        statuses.set_output_ready(request.sid);
        send_video_headers(fd, request, config, prefix);
        response_started = true;
        send_chunk(fd, prefix.data(), prefix.size());
        uint8_t buffer[65536];
        StreamEndReason reason = StreamEndReason::Active;
        while (g_running.load()) {
            const ssize_t count = ffmpeg->read(buffer, sizeof(buffer));
            if (count <= 0) {
                reason = resolve_end_reason(session.get(), ffmpeg.get());
                break;
            }
            send_chunk(fd, buffer, static_cast<size_t>(count));
        }
        if (reason == StreamEndReason::Active) reason = g_running.load() ? resolve_end_reason(session.get(), ffmpeg.get()) : StreamEndReason::ServerStopped;
        publish_status(reason);
        log_line("INFO", "sid=" + request.sid + " 视频管线结束：原因=" + std::string(end_reason_name(reason)) + "，队列当前/峰值=" + std::to_string(session->queue_current_bytes()) + "/" + std::to_string(session->queue_peak_bytes()) + " 字节，背压次数=" + std::to_string(session->queue_backpressure_events()) + "。");
        send_text(fd, "0\r\n\r\n");
    } catch (const ClientDisconnected&) {
        publish_status(StreamEndReason::ClientReplaced);
        throw;
    } catch (const BridgeError&) {
        publish_status(StreamEndReason::StartupFailed);
        throw;
    } catch (...) {
        publish_status(StreamEndReason::StartupFailed);
        throw;
    }
}

void handle_connection(int fd, const Config& config, SessionRegistry& registry, SessionStatusRegistry& statuses, VideoCodecCache& codec_cache) {
    bool response_started = false;
    std::string sid;
    try {
        const auto request = read_http_request(fd);
        if (request.method == "OPTIONS") { send_text(fd, "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nContent-Length: 0\r\n\r\n"); return; }
        if (request.method != "GET") throw BridgeError(405, "METHOD_NOT_ALLOWED", "only GET is supported");
        if (request.path == "/healthz" || request.path == "/") {
            send_json_body(fd, 200, "{\"status\":\"ok\",\"sdk\":\"hcnetsdk\",\"activeSessions\":" + std::to_string(registry.active()) + ",\"hardwareAcceleration\":\"software\"}");
            return;
        }
        if (request.path == "/version") {
            send_json_body(fd, 200, "{\"name\":\"hik-sdk-http-bridge\",\"version\":\"1.0.0\",\"framework\":\"C++17\",\"architecture\":\"linux-amd64\",\"hardwareAcceleration\":\"software\"}");
            return;
        }
        if (request.path == "/session-status") {
            const auto sid_value = request.query.find("sid");
            if (sid_value == request.query.end() || sid_value->second.empty()) throw BridgeError(400, "INVALID_PARAMETER", "sid is required");
            const auto status = statuses.get(sid_value->second);
            if (!status) { send_json(fd, 404, "SESSION_NOT_FOUND", "session status is unavailable", sid_value->second); return; }
            send_session_status(fd, *status);
            return;
        }
        if (request.path == "/session-rendered") {
            const auto sid_value = request.query.find("sid");
            if (sid_value == request.query.end() || sid_value->second.empty()) throw BridgeError(400, "INVALID_PARAMETER", "sid is required");
            long long client_elapsed_ms = 0;
            const auto elapsed_value = request.query.find("elapsedMs");
            try { if (elapsed_value != request.query.end()) client_elapsed_ms = std::stoll(elapsed_value->second); }
            catch (...) { throw BridgeError(400, "INVALID_PARAMETER", "elapsedMs is invalid"); }
            if (client_elapsed_ms < 0 || client_elapsed_ms > 600000) throw BridgeError(400, "INVALID_PARAMETER", "elapsedMs is invalid");
            const auto timing = statuses.set_browser_first_frame(sid_value->second, client_elapsed_ms);
            if (!timing) { send_json(fd, 404, "SESSION_NOT_FOUND", "session status is unavailable", sid_value->second); return; }
            log_line("INFO", "sid=" + sid_value->second + " 浏览器首帧已渲染：服务端总耗时=" + std::to_string(timing->first) + "ms，视频输出至渲染=" + std::to_string(timing->second) + "ms，浏览器上报=" + std::to_string(client_elapsed_ms) + "ms。");
            send_json_body(fd, 200, "{\"status\":\"ok\"}");
            return;
        }
        if (request.path != "/video") throw BridgeError(404, "NOT_FOUND", "endpoint not found");
        const auto stream_request = parse_stream_request(request.query, config);
        sid = stream_request.sid;
        log_line("INFO", "sid=" + sid + " 收到视频播放请求。");
        handle_video(fd, stream_request, config, registry, statuses, codec_cache, response_started);
    } catch (const ClientDisconnected&) {
        log_line("INFO", "sid=" + sid + " 浏览器已主动停止或替换视频流，开始释放资源。");
    } catch (const BridgeError& error) {
        log_line("WARN", "sid=" + sid + " 请求失败：错误码=" + error.code + "，HTTP 状态=" + std::to_string(error.http_status) + "。");
        if (!response_started) { try { send_json(fd, error.http_status, error.code, error.what(), sid); } catch (...) {} }
    } catch (const std::exception& error) {
        log_line("ERROR", std::string("sid=") + sid + " 出现未处理异常。");
        if (!response_started) { try { send_json(fd, 500, "INTERNAL_ERROR", "internal server error", sid); } catch (...) {} }
    }
}

int create_listener(const Config& config) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* addresses = nullptr;
    if (::getaddrinfo(config.bind.c_str(), std::to_string(config.port).c_str(), &hints, &addresses) != 0) throw BridgeError(500, "BIND_FAILED", "invalid bind address");
    int listener = -1;
    for (auto* current = addresses; current; current = current->ai_next) {
        listener = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (listener < 0) continue;
        const int on = 1;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        if (::bind(listener, current->ai_addr, current->ai_addrlen) == 0 && ::listen(listener, 32) == 0) break;
        ::close(listener); listener = -1;
    }
    ::freeaddrinfo(addresses);
    if (listener < 0) throw BridgeError(500, "BIND_FAILED", "cannot bind HTTP listener");
    return listener;
}

void run_server(const Config& config) {
    HcNetRuntime sdk(config);
    SessionRegistry registry(config.max_sessions);
    SessionStatusRegistry statuses;
    VideoCodecCache codec_cache(config.codec_cache_seconds, config.codec_cache_file);
    const int listener = create_listener(config);
    log_line("INFO", "HTTP 视频桥接服务已启动：地址=http://" + config.bind + ":" + std::to_string(config.port) + "。");
    while (g_running.load()) {
        sockaddr_storage peer{};
        socklen_t length = sizeof(peer);
        const int client = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &length);
        if (client < 0) { if (errno == EINTR) continue; log_line("WARN", "接受 HTTP 连接失败：系统错误=" + std::to_string(errno) + "。"); continue; }
        std::thread([client, &config, &registry, &statuses, &codec_cache] { handle_connection(client, config, registry, statuses, codec_cache); ::close(client); }).detach();
    }
    ::close(listener);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1 && (std::string(argv[1]) == "version" || std::string(argv[1]) == "--version")) { std::cout << "hik-sdk-http-bridge 1.0.0\n"; return 0; }
        fs::path config_path{"/opt/hik-bridge/config/config.json"};
        bool validate_config_only = false;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "run") continue;
            if (argument == "validate-config" || argument == "--validate-config") { validate_config_only = true; continue; }
            if (argument == "--config") {
                if (++index >= argc) throw BridgeError(400, "INVALID_COMMAND", "");
                config_path = argv[index];
                continue;
            }
            throw BridgeError(400, "INVALID_COMMAND", "");
        }
        const Config config = load_config(config_path);
        if (!fs::exists(config.sdk_directory / "libhcnetsdk.so")) throw BridgeError(500, "SDK_NOT_FOUND", "");
        if (!fs::exists(config.ffmpeg_path)) throw BridgeError(500, "FFMPEG_NOT_FOUND", "");
        if (validate_config_only) { std::cout << "配置校验通过：" << fs::absolute(config_path).string() << "\n"; return 0; }
        g_daily_logger = std::make_unique<DailyLogWriter>(config.log_directory, config.log_retention_days);
        ::signal(SIGINT, on_signal);
        ::signal(SIGTERM, on_signal);
        ::signal(SIGPIPE, SIG_IGN);
        run_server(config);
        return 0;
    } catch (const BridgeError& error) {
        const bool config_error = error.code == "CONFIG_NOT_FOUND" || error.code == "INVALID_CONFIG" || error.code == "INVALID_COMMAND" || error.code == "SDK_NOT_FOUND" || error.code == "FFMPEG_NOT_FOUND";
        log_line("ERROR", std::string(config_error ? "配置错误：错误码=" : "服务启动失败：错误码=") + error.code + "。");
        return 2;
    } catch (const std::exception& error) {
        log_line("ERROR", "服务启动时发生未处理异常。");
        return 1;
    }
}
