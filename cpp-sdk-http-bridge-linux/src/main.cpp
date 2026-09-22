#include "version.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <signal.h>
#include "windows-service.h"
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <spawn.h>
extern char **environ;
#endif

#include "HCNetSDK.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <clocale>
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
#include <functional>
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

#ifndef HIK_BRIDGE_ARCH
#if defined(_WIN32) && defined(_M_IX86)
#define HIK_BRIDGE_ARCH "windows-x86"
#elif defined(_WIN32) && defined(_M_X64)
#define HIK_BRIDGE_ARCH "windows-x64"
#elif defined(__aarch64__)
#define HIK_BRIDGE_ARCH "linux-arm64"
#elif defined(__x86_64__)
#define HIK_BRIDGE_ARCH "linux-amd64"
#else
#define HIK_BRIDGE_ARCH "linux-unknown"
#endif
#endif

namespace {

#if defined(_WIN32)
using socket_handle = SOCKET;
using socket_length = int;
using io_size = SSIZE_T;
using platform_pollfd = WSAPOLLFD;
constexpr socket_handle invalid_socket = INVALID_SOCKET;
constexpr int platform_pollin = POLLRDNORM;
constexpr int platform_pollout = POLLWRNORM;
int socket_last_error() { return WSAGetLastError(); }
void close_socket(socket_handle value) { if (value != invalid_socket) ::closesocket(value); }
int poll_socket(platform_pollfd* value, ULONG count, int timeout_ms) { return ::WSAPoll(value, count, timeout_ms); }
bool socket_would_block(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
std::string address_error_text(int error) { return std::to_string(error); }
void local_time_safe(std::time_t value, std::tm& output) { localtime_s(&output, &value); }
void utc_time_safe(std::time_t value, std::tm& output) { gmtime_s(&output, &value); }

class NetworkRuntime {
public:
    NetworkRuntime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
        initialized_ = true;
    }
    ~NetworkRuntime() { if (initialized_) WSACleanup(); }
private:
    bool initialized_{false};
};

// Console code pages belong to the shared console, not just this process. In validate-config mode
// the bridge is launched from a CP936 batch file, so restore the caller's code pages on every exit.
class ConsoleCodePageScope {
public:
    ConsoleCodePageScope() : input_(GetConsoleCP()), output_(GetConsoleOutputCP()) {}
    ~ConsoleCodePageScope() {
        if (input_ != 0) SetConsoleCP(input_);
        if (output_ != 0) SetConsoleOutputCP(output_);
    }
private:
    UINT input_;
    UINT output_;
};

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return std::wstring(value.begin(), value.end());
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring quote_windows_argument(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring result{L"\""};
    size_t slashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(ch);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

void write_utf8_line(HANDLE console, FILE* redirected, const std::string& text) {
    DWORD mode = 0;
    if (console != INVALID_HANDLE_VALUE && console != nullptr && GetConsoleMode(console, &mode)) {
        const auto wide = utf8_to_wide(text + "\n");
        DWORD written = 0;
        WriteConsoleW(console, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
        return;
    }
    std::fwrite(text.data(), 1, text.size(), redirected);
    std::fwrite("\n", 1, 1, redirected);
    std::fflush(redirected);
}
void write_stderr_line(const std::string& text) { write_utf8_line(GetStdHandle(STD_ERROR_HANDLE), stderr, text); }
void write_stdout_line(const std::string& text) { write_utf8_line(GetStdHandle(STD_OUTPUT_HANDLE), stdout, text); }
#else
using socket_handle = int;
using socket_length = socklen_t;
using io_size = ssize_t;
using platform_pollfd = pollfd;
constexpr socket_handle invalid_socket = -1;
constexpr int platform_pollin = POLLIN;
constexpr int platform_pollout = POLLOUT;
int socket_last_error() { return errno; }
void close_socket(socket_handle value) { if (value != invalid_socket) ::close(value); }
int poll_socket(platform_pollfd* value, nfds_t count, int timeout_ms) { return ::poll(value, count, timeout_ms); }
bool socket_would_block(int error) { return error == EINPROGRESS || error == EAGAIN || error == EWOULDBLOCK; }
std::string address_error_text(int error) { return ::gai_strerror(error); }
void local_time_safe(std::time_t value, std::tm& output) { localtime_r(&value, &output); }
void utc_time_safe(std::time_t value, std::tm& output) { gmtime_r(&value, &output); }
class NetworkRuntime {};
void write_stderr_line(const std::string& text) { std::cerr << text << std::endl; }
void write_stdout_line(const std::string& text) { std::cout << text << std::endl; }
#endif

// Use atomic close-on-exec creation on Linux: another worker may spawn at any time.
socket_handle create_socket(int family, int type, int protocol) {
#if defined(_WIN32)
    return ::socket(family, type, protocol);
#else
    return ::socket(family, type | SOCK_CLOEXEC, protocol);
#endif
}
socket_handle accept_client(socket_handle listener, sockaddr* peer, socket_length* length) {
#if defined(_WIN32)
    return ::accept(listener, peer, length);
#else
    return ::accept4(listener, peer, length, SOCK_CLOEXEC | SOCK_NONBLOCK);
#endif
}

std::atomic_bool g_running{true};

void on_signal(int) { g_running.store(false); }

void check_running() {
    if (!g_running.load()) throw std::runtime_error("Server is stopping");
}

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
        local_time_safe(now, value);
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
    local_time_safe(now, tm);
    const char* display_level = std::strcmp(level, "WARN") == 0 ? "警告" :
        (std::strcmp(level, "ERROR") == 0 ? "错误" : "信息");
    std::ostringstream line;
    line << std::put_time(&tm, "%F %T") << " [" << display_level << "] " << text;
    write_stderr_line(line.str());
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

// Called by the connection owner at cancellable wait points, never by SDK callbacks.
void check_video_client(socket_handle fd) {
    if (!g_running.load()) throw ClientDisconnected();
    platform_pollfd state{fd, static_cast<short>(platform_pollin), 0};
    const int ready = poll_socket(&state, 1, 0);
    if (ready < 0) {
#if !defined(_WIN32)
        if (errno == EINTR) return;
#endif
        throw ClientDisconnected();
    }
    if (ready == 0) return;
    if (state.revents & (POLLERR | POLLHUP | POLLNVAL)) throw ClientDisconnected();
    if (state.revents & platform_pollin) {
        char byte;
        const int count = ::recv(fd, &byte, 1, MSG_PEEK);
        if (count == 0) throw ClientDisconnected();
        if (count < 0 && !socket_would_block(socket_last_error())) {
#if !defined(_WIN32)
            if (errno == EINTR) return;
#endif
            throw ClientDisconnected();
        }
    }
}

struct Config {
    std::string bind{"127.0.0.1"};
    int port{28080};
    int max_sessions{16};
    int max_http_connections{64};
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
    // Windows 下 auto 依次真实探测 NVENC、QSV、AMF；Linux 始终使用软件转码。
    std::string hardware_acceleration{"auto"};
    int hardware_probe_timeout_ms{3000};
    fs::path log_directory{"../logs"};
    int log_retention_days{30};
    size_t queue_bytes{64 * 1024 * 1024};
    int queue_backpressure_ms{500};
    int max_playback_seconds{86400};
    int sdk_start_ms{10000};
    int first_media_ms{15000};
    int no_sdk_data_ms{10000};
    int output_stall_ms{60000};
    int client_write_stall_ms{60000};
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
    if (!input) throw BridgeError(500, "CONFIG_NOT_FOUND", "未找到配置文件：" + path.string());
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
    catch (...) { throw BridgeError(500, "INVALID_CONFIG", "配置项不是有效整数：" + name); }
}

bool json_bool(const std::string& section, const std::string& name, bool fallback) {
    const auto value = json_value(section, name);
    if (!value) return fallback;
    if (*value == "true") return true;
    if (*value == "false") return false;
    throw BridgeError(500, "INVALID_CONFIG", "配置项不是有效布尔值：" + name);
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
    config.max_http_connections = json_int(server, "maxHttpConnections", config.max_http_connections);
    config.sdk_directory = json_string(sdk, "directory", config.sdk_directory.string());
    config.read_video_codec_from_device = json_bool(sdk, "readVideoCodecFromDevice", config.read_video_codec_from_device);
    config.force_keyframe_on_realplay = json_bool(sdk, "forceKeyFrameOnRealPlay", config.force_keyframe_on_realplay);
    config.realplay_keyframe_interval_frames = json_int(sdk, "realPlayKeyFrameIntervalFrames", config.realplay_keyframe_interval_frames);
    config.connect_probe_timeout_ms = json_int(sdk, "connectProbeTimeoutMs", config.connect_probe_timeout_ms);
    config.ffmpeg_path = json_string(ffmpeg, "path", config.ffmpeg_path.string());
    config.ffmpeg_log_level = json_string(ffmpeg, "logLevel", config.ffmpeg_log_level);
    config.hardware_acceleration = json_string(ffmpeg, "hardwareAcceleration", config.hardware_acceleration);
    std::transform(config.hardware_acceleration.begin(), config.hardware_acceleration.end(), config.hardware_acceleration.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    config.hardware_probe_timeout_ms = json_int(ffmpeg, "hardwareProbeTimeoutMs", config.hardware_probe_timeout_ms);
    config.log_directory = json_string(logging, "directory", config.log_directory.string());
    config.log_retention_days = json_int(logging, "retentionDays", config.log_retention_days);
    const int queue_bytes = json_int(media, "queueBytes", static_cast<int>(config.queue_bytes));
    if (queue_bytes < 0) throw BridgeError(500, "INVALID_CONFIG", "media.queueBytes 必须为正数");
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
    config.output_stall_ms = json_int(timeouts, "outputStallMs", config.output_stall_ms);
    config.client_write_stall_ms = json_int(timeouts, "clientWriteStallMs", config.client_write_stall_ms);
    config.playback_keep_alive_ms = json_int(timeouts, "playbackKeepAliveMs", config.playback_keep_alive_ms);
    if (config.sdk_directory.is_relative()) config.sdk_directory = fs::absolute(root / config.sdk_directory);
    if (config.codec_cache_file.is_relative()) config.codec_cache_file = fs::absolute(root / config.codec_cache_file);
    if (config.ffmpeg_path.is_relative()) config.ffmpeg_path = fs::absolute(root / config.ffmpeg_path);
    if (config.log_directory.is_relative()) config.log_directory = fs::absolute(root / config.log_directory);
    // 本地开发或非标准镜像布局可通过环境变量覆盖，不影响默认 OCI 配置。
    if (const char* sdk_override = std::getenv("HIK_BRIDGE_SDK_DIR")) config.sdk_directory = sdk_override;
    if (const char* ffmpeg_override = std::getenv("HIK_BRIDGE_FFMPEG_PATH")) config.ffmpeg_path = ffmpeg_override;
    if (const char* log_override = std::getenv("HIK_BRIDGE_LOG_DIR")) config.log_directory = log_override;
    if (sdk_type != "hcnetsdk" || config.bind != "127.0.0.1" || config.port < 1 || config.port > 65535 || config.max_sessions < 1 || config.max_http_connections < 1 || config.max_http_connections > 4096 || config.queue_bytes < 65536 || config.queue_backpressure_ms < 0 || config.queue_backpressure_ms > 5000 || config.codec_cache_seconds < 0 || config.codec_cache_seconds > 86400 || config.realplay_keyframe_interval_frames < 0 || config.realplay_keyframe_interval_frames > 65535 || config.connect_probe_timeout_ms < 100 || config.connect_probe_timeout_ms > 10000 || config.codec_cache_file.empty() || config.log_retention_days < 1 ||
        config.max_playback_seconds < 1 || config.sdk_start_ms < 1 || config.first_media_ms < 1 || config.no_sdk_data_ms < 1 ||
        config.output_stall_ms < 1000 || config.output_stall_ms > 600000 || config.client_write_stall_ms < 1000 || config.client_write_stall_ms > 600000 ||
        config.playback_keep_alive_ms < 1000 || config.playback_keep_alive_ms > 5000 ||
        config.log_directory.empty() || config.output_video_codec != "h264" || config.video_encoder.empty() || config.video_preset.empty() ||
        config.hardware_probe_timeout_ms < 1000 || config.hardware_probe_timeout_ms > 30000 ||
        (config.hardware_acceleration != "auto" && config.hardware_acceleration != "off" && config.hardware_acceleration != "none" &&
         config.hardware_acceleration != "disabled" && config.hardware_acceleration != "software" && config.hardware_acceleration != "nvidia" &&
         config.hardware_acceleration != "nvenc" && config.hardware_acceleration != "cuda" && config.hardware_acceleration != "qsv" &&
         config.hardware_acceleration != "intel" && config.hardware_acceleration != "amf" && config.hardware_acceleration != "amd")) {
        throw BridgeError(500, "INVALID_CONFIG", "桥接服务配置无效");
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
    if (item == query.end() || item->second.empty()) throw BridgeError(400, "INVALID_PARAMETER", "缺少必填参数：" + name);
    return item->second;
}

bool is_safe_session_id(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_';
    });
}

long long parse_long(const std::string& value, const std::string& name) {
    try { size_t used = 0; const auto result = std::stoll(value, &used); if (used != value.size()) throw std::invalid_argument("tail"); return result; }
    catch (...) { throw BridgeError(400, "INVALID_PARAMETER", name + " 必须为整数"); }
}

int parse_int(const std::string& value, const std::string& name) {
    const auto result = parse_long(value, name);
    if (result < INT32_MIN || result > INT32_MAX) throw BridgeError(400, "INVALID_PARAMETER", name + " 超出允许范围");
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
    std::string generation;
};

// Windows 的完整 FFmpeg 已验证可稳定封装 H.264 回放；Linux 的裁剪版静态 FFmpeg
// 在 NVR 回放 PS 流直通时可能生成浏览器无法解码的 fMP4。Linux 仅对实时预览保留零转码。
bool h264_copy_allowed(const StreamRequest& request) {
    if (request.speed != 1) return false;
#if defined(_WIN32)
    return true;
#else
    return request.option == Option::RealPlay;
#endif
}

bool linux_h264_playback_transcode_policy(const StreamRequest& request) {
#if defined(_WIN32)
    (void)request;
    return false;
#else
    return request.option == Option::Playback;
#endif
}

StreamRequest parse_stream_request(const Query& query, const Config& config) {
    StreamRequest request{};
    const auto option = required(query, "option");
    if (option == "realplay") request.option = Option::RealPlay;
    else if (option == "playback") request.option = Option::Playback;
    else throw BridgeError(400, "INVALID_PARAMETER", "option 必须为 realplay 或 playback");
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
    else throw BridgeError(400, "INVALID_PARAMETER", "channelType 必须为 auto、analog、digital、digitalIndex 或 ip");
    const auto stream = query.count("stream") ? query.at("stream") : "main";
    if (stream == "main") request.stream = Stream::Main;
    else if (stream == "sub") request.stream = Stream::Sub;
    else throw BridgeError(400, "INVALID_PARAMETER", "stream 必须为 main 或 sub");
    const auto speed_text = query.count("speed") ? query.at("speed") : "1";
    try { request.speed = std::stod(speed_text); } catch (...) { throw BridgeError(400, "INVALID_PARAMETER", "不支持的播放速度"); }
    const std::set<double> valid_speeds{16, 8, 4, 2, 1, .5, .25, .125, .0625};
    if (!valid_speeds.count(request.speed)) throw BridgeError(400, "INVALID_PARAMETER", "不支持的播放速度");
    if (request.port < 1 || request.port > 65535 || request.camera < 0 || (request.channel_type != ChannelType::DigitalIndex && request.camera < 1) || request.ip.size() > 128 || request.username.size() > 63 || request.password.size() > 63 || request.sid.size() > 128 || !is_safe_session_id(request.sid)) {
        throw BridgeError(400, "INVALID_PARAMETER", "NVR 参数超出允许范围");
    }
    if (request.option == Option::Playback) {
        request.start = parse_long(required(query, "start"), "start");
        request.end = parse_long(required(query, "end"), "end");
        if (request.start < 0 || request.end <= request.start || request.end - request.start > config.max_playback_seconds) {
            throw BridgeError(400, "INVALID_PARAMETER", "回放时间范围无效");
        }
    } else if (request.speed != 1) {
        throw BridgeError(400, "INVALID_PARAMETER", "实时预览仅支持 speed=1");
    }
    if (query.count("generation")) request.generation = query.at("generation");
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
    local_time_safe(local_seconds, value);
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
    ManualCleanup,
    PlaybackEnd,
    PlaybackPositionError,
    QueueOverflow,
    SdkDataTimeout,
    SdkKeepAliveFailed,
    FfmpegExit,
    FfmpegOutputTimeout,
    ClientWriteTimeout,
    ClientReplaced,
    ServerStopped,
    StartupFailed,
    StreamEnded
};

const char* end_reason_name(StreamEndReason reason) {
    switch (reason) {
        case StreamEndReason::ManualCleanup: return "manual_cleanup";
        case StreamEndReason::PlaybackEnd: return "playback_end";
        case StreamEndReason::PlaybackPositionError: return "playback_position_error";
        case StreamEndReason::QueueOverflow: return "queue_overflow";
        case StreamEndReason::SdkDataTimeout: return "sdk_data_timeout";
        case StreamEndReason::SdkKeepAliveFailed: return "sdk_keepalive_failed";
        case StreamEndReason::FfmpegExit: return "ffmpeg_exit";
        case StreamEndReason::FfmpegOutputTimeout: return "ffmpeg_output_timeout";
        case StreamEndReason::ClientWriteTimeout: return "client_write_timeout";
        case StreamEndReason::ClientReplaced: return "client_replaced";
        case StreamEndReason::ServerStopped: return "server_stopped";
        case StreamEndReason::StartupFailed: return "startup_failed";
        case StreamEndReason::StreamEnded: return "stream_ended";
        default: return "active";
    }
}

std::string end_reason_message(StreamEndReason reason) {
    switch (reason) {
        case StreamEndReason::ManualCleanup: return "播放已被清理，请点击播放重新连接。";
        case StreamEndReason::PlaybackEnd: return "回放已到达请求的结束时间";
        case StreamEndReason::PlaybackPositionError: return "HCNetSDK 回放位置状态异常";
        case StreamEndReason::QueueOverflow: return "HCNetSDK 媒体队列在限定背压后仍持续满载";
        case StreamEndReason::SdkDataTimeout: return "HCNetSDK 未继续输出媒体数据";
        case StreamEndReason::SdkKeepAliveFailed: return "HCNetSDK 回放保活连续失败";
        case StreamEndReason::FfmpegExit: return "FFmpeg 在视频结束前退出";
        case StreamEndReason::FfmpegOutputTimeout: return "FFmpeg 持续无输出，会话已结束";
        case StreamEndReason::ClientWriteTimeout: return "浏览器持续未接收视频，会话已结束";
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
#if defined(_WIN32)
        const auto library = sdk / "HCNetSDK.dll";
        if (!fs::exists(library)) throw BridgeError(500, "SDK_NOT_FOUND", "目录中缺少 HCNetSDK.dll：" + sdk.string());
        if (!SetDllDirectoryW(sdk.wstring().c_str())) throw BridgeError(500, "SDK_LOAD_FAILED", "设置 HCNetSDK 目录失败，系统错误=" + std::to_string(GetLastError()));
#else
        const auto crypto = sdk / "libcrypto.so.3";
        const auto ssl = sdk / "libssl.so.3";
        if (!fs::exists(sdk / "libhcnetsdk.so") || !fs::exists(crypto) || !fs::exists(ssl)) {
            throw BridgeError(500, "SDK_NOT_FOUND", "HCNetSDK 库文件不完整：" + sdk.string());
        }
        NET_DVR_LOCAL_SDK_PATH sdk_path{};
        std::snprintf(sdk_path.sPath, sizeof(sdk_path.sPath), "%s", sdk.c_str());
        NET_DVR_SetSDKInitCfg(NET_SDK_INIT_CFG_SDK_PATH, &sdk_path);
        NET_DVR_SetSDKInitCfg(NET_SDK_INIT_CFG_LIBEAY_PATH, const_cast<char*>(crypto.c_str()));
        NET_DVR_SetSDKInitCfg(NET_SDK_INIT_CFG_SSLEAY_PATH, const_cast<char*>(ssl.c_str()));
#endif
        NET_DVR_SetConnectTime(3000, 2);
        NET_DVR_SetRecvTimeOut(5000);
        NET_DVR_SetReconnect(10000, TRUE);
        if (!NET_DVR_Init()) throw BridgeError(500, "SDK_INIT_FAILED", "NET_DVR_Init 初始化失败，错误码=" + std::to_string(NET_DVR_GetLastError()));
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
            if (!analog) throw BridgeError(422, "ANALOG_CHANNEL_OUT_OF_RANGE", "camera=" + std::to_string(camera) + " 不在 NVR 模拟通道范围 " + channel_range(analog_start_, analog_count_) + " 内");
            ensure_analog_enabled(camera);
            resolved_type = ResolvedChannelType::Analog;
            return camera;
        }
        if (requested_type == ChannelType::Digital) {
            if (digital_count_ <= 0) throw BridgeError(422, "DIGITAL_CHANNEL_UNSUPPORTED", "NVR 未报告数字/IP 通道");
            if (!digital) throw BridgeError(422, "DIGITAL_CHANNEL_OUT_OF_RANGE", "camera=" + std::to_string(camera) + " 不在 NVR 数字/IP 通道范围 " + channel_range(digital_start_, digital_count_) + " 内");
            resolved_type = ResolvedChannelType::Digital;
            return camera;
        }
        if (requested_type == ChannelType::DigitalIndex) {
            if (digital_count_ <= 0) throw BridgeError(422, "DIGITAL_CHANNEL_UNSUPPORTED", "NVR 未报告数字/IP 通道");
            if (camera < 0 || camera >= digital_count_) throw BridgeError(422, "DIGITAL_CHANNEL_INDEX_OUT_OF_RANGE", "camera=" + std::to_string(camera) + " 不在从零开始的数字通道序号范围 0-" + std::to_string(digital_count_ - 1) + " 内");
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
        throw BridgeError(422, "CHANNEL_NOT_FOUND", "NVR 未报告 camera=" + std::to_string(camera) + "；模拟通道=" + channel_range(analog_start_, analog_count_) + "，数字通道=" + channel_range(digital_start_, digital_count_));
    }

private:
    bool is_analog(int camera) const { return analog_count_ > 0 && camera >= analog_start_ && camera < analog_start_ + analog_count_; }
    bool is_digital(int camera) const { return digital_count_ > 0 && camera >= digital_start_ && camera < digital_start_ + digital_count_; }
    void ensure_analog_enabled(int camera) const {
        const int index = camera - analog_start_;
        if (ip_parameter_config_read_ && (index < 0 || index >= MAX_CHANNUM_V30 || !analog_enabled_[static_cast<size_t>(index)])) {
            throw BridgeError(422, "ANALOG_CHANNEL_DISABLED", "camera=" + std::to_string(camera) + " 是模拟通道，但已禁用或 NVR 未配置模拟输入");
        }
    }
    BridgeError unsupported_analog(int camera) const {
        const auto start = digital_start_ > 0 ? std::to_string(digital_start_) : "33";
        return BridgeError(422, "ANALOG_CHANNEL_UNSUPPORTED", "camera=" + std::to_string(camera) + " 被指定为模拟通道，但 NVR 未报告模拟通道；IP 摄像机请使用 NVR 的数字/IP SDK 通道号（通常从 " + start + " 开始）");
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
        log_line("WARN", "sid=" + request.sid + " NVR SDK 地址解析失败：设备=" + target + "，原因=" + address_error_text(resolve_error) + "。");
        throw BridgeError(502, "NVR_UNREACHABLE", "无法连接 NVR " + target + " 的 SDK 端口；请检查设备 IP、SDK 端口、网络连通性或防火墙。");
    }

    int last_error = ETIMEDOUT;
    bool connected = false;
    for (addrinfo* item = addresses; item != nullptr && !connected; item = item->ai_next) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        const int remaining = timeout_ms - static_cast<int>(elapsed);
        if (remaining <= 0) break;

        const socket_handle fd = create_socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd == invalid_socket) {
            last_error = socket_last_error();
            continue;
        }
#if defined(_WIN32)
        u_long nonblocking = 1;
        if (::ioctlsocket(fd, FIONBIO, &nonblocking) != 0) {
            last_error = socket_last_error();
            close_socket(fd);
            continue;
        }
#else
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            last_error = errno;
            close_socket(fd);
            continue;
        }
#endif
        if (::connect(fd, item->ai_addr, item->ai_addrlen) == 0) {
            connected = true;
            close_socket(fd);
            break;
        }
        const int connect_error = socket_last_error();
        if (socket_would_block(connect_error)) {
            platform_pollfd poll_fd{};
            poll_fd.fd = fd;
            poll_fd.events = platform_pollout;
            const int poll_result = poll_socket(&poll_fd, 1, remaining);
            if (poll_result > 0) {
                socket_length length = sizeof(last_error);
#if defined(_WIN32)
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&last_error), &length) == 0 && last_error == 0) connected = true;
#else
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &last_error, &length) == 0 && last_error == 0) connected = true;
#endif
            } else {
                last_error = poll_result == 0 ?
#if defined(_WIN32)
                    WSAETIMEDOUT
#else
                    ETIMEDOUT
#endif
                    : socket_last_error();
            }
        } else {
            last_error = connect_error;
        }
        close_socket(fd);
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
    HcSession(const StreamRequest& request, const Config& config, std::function<void()> check_client = {})
        : request_(request), config_(config), queue_(config.queue_bytes), check_client_(std::move(check_client)) {}
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
        check_request();
        if (prepared_) return;
        probe_nvr_endpoint(request_, config_.connect_probe_timeout_ms);
        check_request();
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
        check_request();
        if (user_ < 0) {
            const DWORD error = NET_DVR_GetLastError();
            // HCNetSDK 错误 7 表示设备连接失败；前端需要按网络不可达展示，而不是误导为账号错误。
            if (error == 7) throw BridgeError(502, "NVR_UNREACHABLE", "无法连接 NVR " + request_.ip + ":" + std::to_string(request_.port) + " 的 SDK 端口；请检查设备 IP、SDK 端口、网络连通性或防火墙。");
            throw BridgeError(502, "SDK_LOGIN_FAILED", "NVR 登录失败：HCNetSDK 错误=" + std::to_string(error) + "。请检查用户名、密码、设备状态和登录权限。");
        }
        const auto login_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - login_started).count();
        log_line("INFO", "sid=" + request_.sid + " NVR 登录成功：耗时=" + std::to_string(login_elapsed) + "ms。");
        topology_ = NvrChannelTopology::read(user_, info, request_.sid);
        check_request();
        sdk_camera_ = topology_.resolve(request_.camera, request_.channel_type, resolved_channel_type_);
        log_line("INFO", "sid=" + request_.sid + " SDK 通道已解析：请求通道=" + std::to_string(request_.camera) + "，请求类型=" + channel_type_name(request_.channel_type) + "，实际通道=" + std::to_string(sdk_camera_) + "，实际类型=" + resolved_channel_type_name(resolved_channel_type_) + "。");
        read_video_stream_configuration();
        check_request();
        prepared_ = true;
    }

    void start_stream_and_wait_for_data() {
        prepare();
        check_request();
        if (!stream_started_) {
            if (request_.option == Option::RealPlay) start_realplay(); else start_playback();
            stream_started_ = true;
        log_line("INFO", "sid=" + request_.sid + " NVR 通道建流已提交：模式=" + std::string(request_.option == Option::RealPlay ? "实时预览" : "视频回放") + "，请求通道=" + std::to_string(request_.camera) + "，请求类型=" + channel_type_name(request_.channel_type) + "，实际通道=" + std::to_string(sdk_camera_) + "，实际类型=" + resolved_channel_type_name(resolved_channel_type_) + "，码流=" + (request_.stream == Stream::Main ? "主码流" : "子码流") + "，速度=" + std::to_string(request_.speed) + "x。");
        }
        std::unique_lock<std::mutex> lock(first_mutex_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.sdk_start_ms);
        while (!first_data_ && !overflow_) {
            check_request();
            if (std::chrono::steady_clock::now() >= deadline)
                throw BridgeError(504, "SDK_STREAM_TIMEOUT", "等待 HCNetSDK 视频数据超时");
            first_cv_.wait_for(lock, 100ms);
        }
        check_request();
        if (overflow_) throw BridgeError(502, "MEDIA_QUEUE_OVERFLOW", "HCNetSDK 媒体队列超过背压限制");
        log_line("INFO", "sid=" + request_.sid + " HCNetSDK 已获取首个视频数据。");
    }

private:
    void check_request() const { check_running(); if (check_client_) check_client_(); }
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
    std::function<void()> check_client_;
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

bool write_socket_all(socket_handle fd, const uint8_t* data, size_t size, int stall_ms = 60000) {
    auto last_progress = std::chrono::steady_clock::now();
    while (size > 0) {
        if (!g_running.load()) return false;
        const int sent = ::send(fd, reinterpret_cast<const char*>(data), static_cast<int>(std::min<size_t>(size, 64 * 1024)), 0);
        if (sent > 0) { data += sent; size -= static_cast<size_t>(sent); last_progress = std::chrono::steady_clock::now(); continue; }
        if (sent < 0 && socket_would_block(socket_last_error())) {
            check_video_client(fd);
            if (std::chrono::steady_clock::now() - last_progress >= std::chrono::milliseconds(stall_ms))
                throw BridgeError(504, "CLIENT_WRITE_TIMEOUT", "浏览器持续未接收视频，已结束会话");
            platform_pollfd writable{fd, static_cast<short>(platform_pollout), 0};
            poll_socket(&writable, 1, 100);
            continue;
        }
#if !defined(_WIN32)
        if (sent < 0 && errno == EINTR) continue;
#endif
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

enum class HardwareBackend { None, NvidiaNvenc, IntelQsv, AmdAmf };

struct FfmpegTranscodePlan {
    bool hardware_enabled{false};
    std::string backend_name{"software"};
    std::string video_encoder;
};

#if defined(_WIN32)
struct CapturedProcessResult {
    int exit_code{-1};
    bool timed_out{false};
    std::string output;
};

CapturedProcessResult run_windows_process(const fs::path& executable, const std::vector<std::string>& arguments, int timeout_ms) {
    CapturedProcessResult result;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    if (!CreatePipe(&output_read, &output_write, &attributes, 0)) return result;
    SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);

    std::wstring command = quote_windows_argument(executable.wstring());
    for (const auto& argument : arguments) command += L" " + quote_windows_argument(utf8_to_wide(argument));
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = output_write;
    startup.hStdError = output_write;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                        executable.parent_path().c_str(), &startup, &process)) {
        CloseHandle(output_read);
        CloseHandle(output_write);
        return result;
    }
    CloseHandle(output_write);
    CloseHandle(process.hThread);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::array<char, 4096> buffer{};
    bool exited = false;
    while (!exited) {
        DWORD available = 0;
        if (PeekNamedPipe(output_read, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD count = 0;
            if (ReadFile(output_read, buffer.data(), static_cast<DWORD>(std::min<size_t>(buffer.size(), available)), &count, nullptr) && count > 0)
                result.output.append(buffer.data(), count);
        }
        exited = WaitForSingleObject(process.hProcess, 10) == WAIT_OBJECT_0;
        if (!exited && std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            exited = true;
        }
    }
    while (true) {
        DWORD count = 0;
        if (!ReadFile(output_read, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) || count == 0) break;
        result.output.append(buffer.data(), count);
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    CloseHandle(output_read);
    CloseHandle(process.hProcess);
    windows_service_start_progress();
    return result;
}
#endif

class FfmpegHardwareAcceleration {
public:
    static FfmpegHardwareAcceleration detect(const Config& config) {
#if !defined(_WIN32)
        (void)config;
        log_line("INFO", "当前平台=" HIK_BRIDGE_ARCH "，按策略使用 FFmpeg 软件转码。");
        return {};
#else
        const auto requested = normalize(config.hardware_acceleration);
        const auto software_test = run_windows_process(config.ffmpeg_path,
            {"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=c=black:s=64x64:r=1",
             "-frames:v", "1", "-an", "-c:v", config.video_encoder, "-f", "null", "-"},
            config.hardware_probe_timeout_ms);
        if (software_test.timed_out || software_test.exit_code != 0)
            throw BridgeError(500, "FFMPEG_SOFTWARE_ENCODER_UNAVAILABLE", "FFmpeg 软件 H.264 编码器探测失败");
        log_line("INFO", "Windows FFmpeg 软件转码兜底探测成功：编码器=" + config.video_encoder + "。");
        if (requested == "off") {
            log_line("INFO", "Windows FFmpeg 硬件转码已由配置关闭，将使用软件转码。");
            return {};
        }
        const auto hwaccels = run_windows_process(config.ffmpeg_path, {"-hide_banner", "-hwaccels"}, config.hardware_probe_timeout_ms);
        const auto encoders = run_windows_process(config.ffmpeg_path, {"-hide_banner", "-encoders"}, config.hardware_probe_timeout_ms);
        if (hwaccels.timed_out || encoders.timed_out || hwaccels.exit_code != 0 || encoders.exit_code != 0) {
            log_line("WARN", "Windows FFmpeg 硬件能力探测失败或超时，将使用软件转码。");
            return {};
        }
        for (const auto& candidate : candidates(requested)) {
            if (!has_token(hwaccels.output, candidate.method) || !has_token(encoders.output, candidate.encoder)) continue;
            const auto test = run_windows_process(config.ffmpeg_path,
                {"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=c=black:s=64x64:r=1",
                 "-frames:v", "1", "-an", "-c:v", candidate.encoder, "-f", "null", "-"},
                config.hardware_probe_timeout_ms);
            if (!test.timed_out && test.exit_code == 0) {
                FfmpegHardwareAcceleration result;
                result.backend_ = candidate.backend;
                log_line("INFO", "Windows FFmpeg 硬件转码探测成功：后端=" + candidate.name + "，编码器=" + candidate.encoder + "。");
                return result;
            }
            log_line("WARN", "Windows FFmpeg 硬件编码器真实编码测试失败：后端=" + candidate.name + "；继续尝试其他后端。");
        }
        log_line("WARN", "Windows 未检测到可用显卡硬件编码器，将使用 libx264 软件转码。");
        return {};
#endif
    }

    FfmpegTranscodePlan select_plan(VideoOutputMode output_mode, bool force_software = false) const {
        if (output_mode == VideoOutputMode::Copy || force_software || backend_ == HardwareBackend::None) return {};
        switch (backend_) {
            case HardwareBackend::NvidiaNvenc: return {true, "nvidia-nvenc", "h264_nvenc"};
            case HardwareBackend::IntelQsv: return {true, "intel-qsv", "h264_qsv"};
            case HardwareBackend::AmdAmf: return {true, "amd-amf", "h264_amf"};
            default: return {};
        }
    }

    std::string backend_name() const {
        switch (backend_) {
            case HardwareBackend::NvidiaNvenc: return "nvidia-nvenc";
            case HardwareBackend::IntelQsv: return "intel-qsv";
            case HardwareBackend::AmdAmf: return "amd-amf";
            default: return "software";
        }
    }
private:
#if defined(_WIN32)
    struct Candidate { HardwareBackend backend; std::string name; std::string method; std::string encoder; };
    static std::string normalize(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (value == "none" || value == "disabled" || value == "software") return "off";
        if (value == "nvenc" || value == "cuda") return "nvidia";
        if (value == "intel") return "qsv";
        if (value == "amd") return "amf";
        return value;
    }
    static bool has_token(const std::string& text, const std::string& token) {
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream fields(line);
            std::string field;
            while (fields >> field) if (field == token) return true;
        }
        return false;
    }
    static std::vector<Candidate> candidates(const std::string& requested) {
        const Candidate nvidia{HardwareBackend::NvidiaNvenc, "nvidia-nvenc", "cuda", "h264_nvenc"};
        const Candidate qsv{HardwareBackend::IntelQsv, "intel-qsv", "qsv", "h264_qsv"};
        const Candidate amf{HardwareBackend::AmdAmf, "amd-amf", "amf", "h264_amf"};
        if (requested == "nvidia") return {nvidia};
        if (requested == "qsv") return {qsv};
        if (requested == "amf") return {amf};
        return {nvidia, qsv, amf};
    }
#endif
    HardwareBackend backend_{HardwareBackend::None};
};

class FfmpegProcess {
public:
    FfmpegProcess(const Config& config, const StreamRequest& request, VideoOutputMode video_output_mode,
                  const FfmpegHardwareAcceleration& hardware, bool force_software = false, std::function<void()> check_client = {})
        : config_(config), sid_(request.sid), video_output_mode_(video_output_mode),
          transcode_plan_(hardware.select_plan(video_output_mode, force_software)), check_client_(std::move(check_client)),
          output_stall_ms_(static_cast<int>(config.output_stall_ms / std::clamp(request.speed, 0.0625, 1.0))) {
        if (check_client_) check_client_();
        auto arguments = build_arguments(config, request, video_output_mode, transcode_plan_);
        try {
#if defined(_WIN32)
        start_windows(arguments);
#else
        start_linux(arguments);
#endif
        error_pump_ = std::thread([this] { read_diagnostics(); });
        log_line("INFO", "sid=" + sid_ + " FFmpeg 转流进程已启动：模式=" + std::string(video_output_mode == VideoOutputMode::Copy ? "H.264 直通/编码探测" : "转码为 H.264") +
            "，平台=" HIK_BRIDGE_ARCH "，转码后端=" + transcode_plan_.backend_name + "，编码器=" +
            (video_output_mode == VideoOutputMode::Copy ? "copy" : (transcode_plan_.hardware_enabled ? transcode_plan_.video_encoder : config.video_encoder)) +
            "，速度=" + std::to_string(request.speed) + "x，音频=" + std::string(config.enable_audio && request.speed == 1 ? "启用" : "关闭") + "。");
        } catch (...) {
            stop_process(true);
            throw;
        }
    }
    ~FfmpegProcess() { stop_process(false); }
    void stop_for_pipeline_switch() { stop_process(true); }
    bool hardware_enabled() const { return transcode_plan_.hardware_enabled; }
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
                if (!write_input(data.data(), data.size())) break;
            }
            if (!stop_requested_.load() && !queue.closed() && input_end_reason() != StreamEndReason::SdkDataTimeout) input_end_reason_.store(StreamEndReason::SdkDataTimeout);
            if (!stop_requested_.load() && input_end_reason() == StreamEndReason::SdkDataTimeout)
                log_line("WARN", "sid=" + sid_ + " FFmpeg 输入超时：HCNetSDK 在 " + std::to_string(config_.no_sdk_data_ms) + "ms 内未提供数据。");
            close_input();
        });
    }
    std::vector<uint8_t> read_initial(int timeout_ms) {
        return read_until_ready({}, timeout_ms, false);
    }
    std::vector<uint8_t> read_first_media(std::vector<uint8_t> initial_mp4, int timeout_ms) {
        return read_until_ready(std::move(initial_mp4), timeout_ms, true);
    }
    std::string diagnostic_tail() const {
        std::lock_guard<std::mutex> lock(diagnostics_mutex_);
        return diagnostics_tail_;
    }
    io_size read(uint8_t* buffer, size_t size) {
        // Count only time spent waiting for output, not browser backpressure between reads.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(output_stall_ms_);
        while (g_running.load()) {
            if (check_client_) check_client_();
            if (std::chrono::steady_clock::now() >= deadline)
                throw BridgeError(504, "FFMPEG_OUTPUT_TIMEOUT", "FFmpeg 持续无输出，已结束会话");
            const auto count = read_output_timeout(buffer, size, std::chrono::steady_clock::now() + 100ms);
            if (count >= 0) return count;
        }
        return 0;
    }
    StreamEndReason input_end_reason() const { return input_end_reason_.load(); }
private:
    std::vector<uint8_t> read_until_ready(std::vector<uint8_t> result, int timeout_ms, bool require_media_fragment) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        uint8_t buffer[32768];
        while (std::chrono::steady_clock::now() < deadline && result.size() < 4 * 1024 * 1024) {
            check_running();
            if (check_client_) check_client_();
            const auto read_size = read_output_timeout(buffer, sizeof(buffer), deadline);
            if (read_size < 0) continue;
            if (read_size == 0) break;
            result.insert(result.end(), buffer, buffer + read_size);
            const bool initialized = contains_atom(result, "ftyp") && contains_atom(result, "moov");
            if (initialized && (!require_media_fragment || (contains_atom(result, "moof") && contains_atom(result, "mdat")))) return result;
        }
        if (require_media_fragment && contains_atom(result, "ftyp") && contains_atom(result, "moov"))
            throw BridgeError(504, "FFMPEG_NO_MEDIA_FRAGMENT", "FFmpeg 已生成 fMP4 初始化段，但未生成首个 moof/mdat 媒体分片");
        throw BridgeError(502, "FFMPEG_NO_MP4", "FFmpeg 未生成 fragmented MP4 初始化数据");
    }
    static std::vector<std::string> build_arguments(const Config& config, const StreamRequest& request, VideoOutputMode output_mode, const FfmpegTranscodePlan& plan) {
        std::vector<std::string> args{"-hide_banner", "-loglevel", config.ffmpeg_log_level, "-fflags", "+genpts+nobuffer", "-analyzeduration", "250000", "-probesize", "262144", "-i", "pipe:0", "-map", "0:v:0"};
        if (output_mode == VideoOutputMode::Copy) args.insert(args.end(), {"-c:v", "copy"});
        else {
            if (!plan.hardware_enabled) args.insert(args.end(), {"-c:v", config.video_encoder, "-preset", config.video_preset, "-tune", "zerolatency", "-pix_fmt", "yuv420p"});
            else if (plan.video_encoder == "h264_nvenc") args.insert(args.end(), {"-c:v", "h264_nvenc", "-preset", "p4", "-tune", "ll", "-rc", "vbr"});
            else if (plan.video_encoder == "h264_qsv") args.insert(args.end(), {"-c:v", "h264_qsv", "-preset", "veryfast"});
            else if (plan.video_encoder == "h264_amf") args.insert(args.end(), {"-c:v", "h264_amf", "-usage", "lowlatency", "-quality", "speed"});
            args.insert(args.end(), {"-profile:v", "high", "-level:v", "4.1", "-g", "25", "-keyint_min", "25", "-sc_threshold", "0"});
            if (request.speed != 1) {
                std::ostringstream speed;
                speed << std::setprecision(8) << request.speed;
                args.insert(args.end(), {"-vf", "setpts=PTS/" + speed.str()});
            }
        }
        if (config.enable_audio && request.speed == 1) args.insert(args.end(), {"-map", "0:a:0?", "-c:a", "aac", "-ar", "48000", "-ac", "2"});
        else args.insert(args.end(), {"-an"});
        args.insert(args.end(), {"-f", "mp4", "-movflags", "frag_keyframe+empty_moov+default_base_moof+omit_tfhd_offset", "-frag_duration", "250000", "pipe:1"});
        return args;
    }
#if defined(_WIN32)
    void start_windows(const std::vector<std::string>& arguments) {
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        HANDLE input_read = nullptr, output_write = nullptr, error_write = nullptr;
        struct ChildPipeEnds {
            HANDLE& input; HANDLE& output; HANDLE& error;
            ~ChildPipeEnds() { for (HANDLE value : {input, output, error}) if (value) CloseHandle(value); }
        } child_ends{input_read, output_write, error_write};
        if (!CreatePipe(&input_read, &input_write_, &attributes, 0) || !CreatePipe(&output_read_, &output_write, &attributes, 0) ||
            !CreatePipe(&error_read_, &error_write, &attributes, 0)) throw BridgeError(500, "PIPE_FAILED", "无法创建 FFmpeg 管道");
        SetHandleInformation(input_write_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(output_read_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(error_read_, HANDLE_FLAG_INHERIT, 0);
        std::wstring command = quote_windows_argument(config_.ffmpeg_path.wstring());
        for (const auto& argument : arguments) command += L" " + quote_windows_argument(utf8_to_wide(argument));
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        // Do not let another FFmpeg process inherit browser sockets or another session's pipes.
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdInput = input_read;
        startup.StartupInfo.hStdOutput = output_write;
        startup.StartupInfo.hStdError = error_write;
        SIZE_T attribute_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
        std::vector<uint8_t> attribute_storage(attribute_size);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        HANDLE inherited[] = {input_read, output_write, error_write};
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size)) {
            throw BridgeError(500, "FFMPEG_START_FAILED", "无法初始化 FFmpeg 句柄继承列表");
        }
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            throw BridgeError(500, "FFMPEG_START_FAILED", "无法限制 FFmpeg 句柄继承");
        }
        PROCESS_INFORMATION process{};
        const BOOL started = CreateProcessW(config_.ffmpeg_path.c_str(), mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                             config_.ffmpeg_path.parent_path().c_str(), &startup.StartupInfo, &process);
        const DWORD start_error = GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (!started) throw BridgeError(500, "FFMPEG_START_FAILED", "启动 FFmpeg 进程失败，系统错误=" + std::to_string(start_error));
        CloseHandle(process.hThread);
        process_ = process.hProcess;
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (job_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION information{};
            information.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &information, sizeof(information));
            AssignProcessToJobObject(job_, process_);
        }
    }
#else
    void start_linux(std::vector<std::string> arguments) {
        int input_pipe[2]{-1, -1}, output_pipe[2]{-1, -1}, error_pipe[2]{-1, -1};
        struct PipeEnds {
            int* input; int* output; int* error;
            ~PipeEnds() { for (int* pair : {input, output, error}) for (int i = 0; i < 2; ++i) if (pair[i] >= 0) ::close(pair[i]); }
        } pipe_ends{input_pipe, output_pipe, error_pipe};
        for (int* ends : {input_pipe, output_pipe, error_pipe}) {
            if (::pipe2(ends, O_CLOEXEC) != 0) throw BridgeError(500, "PIPE_FAILED", "无法创建 FFmpeg 管道");
            // Keep action sources away from stdio even if the parent was launched with closed stdio.
            for (int i = 0; i < 2; ++i) if (ends[i] < 3) {
                const int raised = ::fcntl(ends[i], F_DUPFD_CLOEXEC, 3);
                if (raised < 0) throw BridgeError(500, "PIPE_FAILED", "无法保护 FFmpeg 标准管道");
                ::close(ends[i]); ends[i] = raised;
            }
        }
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char*>("ffmpeg"));
        for (auto& item : arguments) argv.push_back(item.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) != 0) throw BridgeError(500, "FFMPEG_START_FAILED", "无法初始化 FFmpeg 启动操作");
        struct ActionsGuard { posix_spawn_file_actions_t* value; ~ActionsGuard() { posix_spawn_file_actions_destroy(value); } } actions_guard{&actions};
        const auto check_action = [](int error) {
            if (error != 0) throw BridgeError(500, "FFMPEG_START_FAILED", "无法配置 FFmpeg 管道，错误=" + std::to_string(error));
        };
        check_action(posix_spawn_file_actions_adddup2(&actions, input_pipe[0], STDIN_FILENO));
        check_action(posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO));
        check_action(posix_spawn_file_actions_adddup2(&actions, error_pipe[1], STDERR_FILENO));
        for (int fd : {input_pipe[0], input_pipe[1], output_pipe[0], output_pipe[1], error_pipe[0], error_pipe[1]})
            check_action(posix_spawn_file_actions_addclose(&actions, fd));
        pid_t child = -1;
        const int error = ::posix_spawn(&child, config_.ffmpeg_path.c_str(), &actions, nullptr, argv.data(), ::environ);
        if (error != 0) throw BridgeError(500, "FFMPEG_START_FAILED", "无法启动 FFmpeg，错误=" + std::to_string(error));
        pid_ = child;
        input_fd_ = input_pipe[1]; output_fd_ = output_pipe[0]; error_fd_ = error_pipe[0];
        input_pipe[1] = output_pipe[0] = error_pipe[0] = -1;
    }
#endif
    bool write_input(const uint8_t* data, size_t size) {
        while (size > 0) {
#if defined(_WIN32)
            if (!input_write_) return false;
            DWORD count = 0;
            const DWORD part = static_cast<DWORD>(std::min<size_t>(size, 64 * 1024));
            if (!WriteFile(input_write_, data, part, &count, nullptr) || count == 0) return false;
#else
            const auto count = ::write(input_fd_, data, size);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
#endif
            data += count; size -= static_cast<size_t>(count);
        }
        return true;
    }
    io_size read_output_timeout(uint8_t* buffer, size_t size, std::chrono::steady_clock::time_point deadline) {
#if defined(_WIN32)
        while (std::chrono::steady_clock::now() < deadline) {
            check_running();
            if (check_client_) check_client_();
            DWORD available = 0;
            if (output_read_ && PeekNamedPipe(output_read_, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                DWORD count = 0;
                if (ReadFile(output_read_, buffer, static_cast<DWORD>(std::min<size_t>(size, available)), &count, nullptr)) return static_cast<io_size>(count);
                return 0;
            }
            if (process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) return 0;
            std::this_thread::sleep_for(10ms);
        }
        return -1;
#else
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        platform_pollfd poll_fd{output_fd_, static_cast<short>(platform_pollin), 0};
        if (poll_socket(&poll_fd, 1, static_cast<int>(std::clamp<long long>(remaining, 1, 100))) <= 0) return -1;
        return ::read(output_fd_, buffer, size);
#endif
    }
    void close_input() {
#if defined(_WIN32)
        if (input_write_) { CloseHandle(input_write_); input_write_ = nullptr; }
#else
        if (input_fd_ >= 0) { ::close(input_fd_); input_fd_ = -1; }
#endif
    }
    void stop_process(bool immediate) {
        if (stop_requested_.exchange(true)) return;
        // End the child first to unblock a pump stuck in WriteFile/write. The
        // pump owns input closure until joined; closing its handle concurrently
        // could otherwise race with reuse of that handle/file descriptor.
#if defined(_WIN32)
        if (process_) {
            if (immediate || WaitForSingleObject(process_, 500) == WAIT_TIMEOUT) TerminateProcess(process_, 1);
            WaitForSingleObject(process_, INFINITE);
        }
#else
        if (pid_ > 0) {
            ::kill(pid_, immediate ? SIGKILL : SIGTERM);
            int status = 0;
            const auto deadline = std::chrono::steady_clock::now() + 500ms;
            pid_t waited;
            do {
                waited = ::waitpid(pid_, &status, WNOHANG);
                if (waited == pid_ || (waited < 0 && errno != EINTR)) break;
                std::this_thread::sleep_for(10ms);
            } while (std::chrono::steady_clock::now() < deadline);
            if (waited == 0 || (waited < 0 && errno == EINTR)) {
                ::kill(pid_, SIGKILL);
                while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
            }
            pid_ = -1;
        }
#endif
        if (pump_.joinable()) pump_.join();
        close_input();
        // Process exit closes stderr. Join its reader before closing its handle.
        if (error_pump_.joinable()) error_pump_.join();
#if defined(_WIN32)
        if (output_read_) { CloseHandle(output_read_); output_read_ = nullptr; }
        if (error_read_) { CloseHandle(error_read_); error_read_ = nullptr; }
#else
        if (output_fd_ >= 0) { ::close(output_fd_); output_fd_ = -1; }
        if (error_fd_ >= 0) { ::close(error_fd_); error_fd_ = -1; }
#endif
#if defined(_WIN32)
        if (process_) { CloseHandle(process_); process_ = nullptr; }
        if (job_) { CloseHandle(job_); job_ = nullptr; }
#endif
    }
    void read_diagnostics() {
        std::string pending;
        char buffer[1024];
        while (true) {
            io_size count = 0;
#if defined(_WIN32)
            DWORD read_count = 0;
            DWORD available = 0;
            if (!error_read_ || !PeekNamedPipe(error_read_, nullptr, 0, nullptr, &available, nullptr)) break;
            if (!available) {
                if (stop_requested_.load()) break;
                std::this_thread::sleep_for(10ms);
                continue;
            }
            if (!error_read_ || !ReadFile(error_read_, buffer, sizeof(buffer), &read_count, nullptr)) break;
            count = static_cast<io_size>(read_count);
#else
            if (error_fd_ < 0) break;
            platform_pollfd diagnostic_poll{error_fd_, static_cast<short>(platform_pollin), 0};
            if (poll_socket(&diagnostic_poll, 1, 100) <= 0) {
                if (stop_requested_.load()) break;
                continue;
            }
            count = ::read(error_fd_, buffer, sizeof(buffer));
#endif
            if (count <= 0) break;
            pending.append(buffer, static_cast<size_t>(count));
            size_t end = 0;
            while ((end = pending.find_first_of("\r\n")) != std::string::npos) {
                const auto line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (!line.empty()) {
                    std::lock_guard<std::mutex> lock(diagnostics_mutex_);
                    diagnostics_tail_.append(line).push_back('\n');
                    if (diagnostics_tail_.size() > 8192) diagnostics_tail_.erase(0, diagnostics_tail_.size() - 8192);
                }
                if (line.find("Video: hevc") != std::string::npos || line.find("Video: h265") != std::string::npos)
                    log_line("INFO", "sid=" + sid_ + " FFmpeg 已识别输入视频编码=H.265，处理方式=" + std::string(video_output_mode_ == VideoOutputMode::Copy ? "编码探测" : "转码为 H.264") + "。");
                else if (line.find("Video: h264") != std::string::npos || line.find("Video: avc") != std::string::npos)
                    log_line("INFO", "sid=" + sid_ + " FFmpeg 已识别输入视频编码=H.264，处理方式=" + std::string(video_output_mode_ == VideoOutputMode::Copy ? "H.264 直通" : "转码为 H.264") + "。");
            }
        }
    }
    static bool contains_atom(const std::vector<uint8_t>& data, const char* atom) { return std::search(data.begin(), data.end(), atom, atom + 4) != data.end(); }
    const Config& config_;
    std::string sid_;
    VideoOutputMode video_output_mode_;
    FfmpegTranscodePlan transcode_plan_;
    std::function<void()> check_client_;
    int output_stall_ms_;
#if defined(_WIN32)
    HANDLE process_{nullptr}, job_{nullptr}, input_write_{nullptr}, output_read_{nullptr}, error_read_{nullptr};
#else
    pid_t pid_{-1};
    int input_fd_{-1}, output_fd_{-1}, error_fd_{-1};
#endif
    std::thread pump_;
    std::thread error_pump_;
    mutable std::mutex diagnostics_mutex_;
    std::string diagnostics_tail_;
    std::atomic_bool stop_requested_{false};
    std::atomic<StreamEndReason> input_end_reason_{StreamEndReason::Active};
};

class SessionRegistry {
public:
    explicit SessionRegistry(int max) : max_(max) {}
    struct Counts { int active; int maximum; int available; };
    struct State { std::string sid; socket_handle fd; std::string generation; std::atomic_bool cancelled{false}; };
    class Lease {
    public:
        Lease(SessionRegistry* owner, std::shared_ptr<State> value) : state(std::move(value)), owner_(owner) {}
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() { owner_->release(state); }
        bool cancelled() const { return state->cancelled.load(); }
        std::shared_ptr<State> state;
    private:
        SessionRegistry* owner_;
    };
    std::unique_ptr<Lease> reserve(const std::string& sid = "", socket_handle fd = invalid_socket,
                                 const std::string& generation = "", std::function<void()> initialize = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Legacy clients can play before the first cleanup only. Never let a missing token bypass a cleanup.
        if ((generation.empty() && cleaned_) || (!generation.empty() && generation != generation_))
            throw BridgeError(409, "MANUAL_RESTART_REQUIRED", "播放已被清理，请点击播放重新连接。");
        if (states_.size() >= static_cast<size_t>(max_)) return {};
        auto state = std::make_shared<State>(); state->sid = sid; state->fd = fd; state->generation = generation_;
        if (initialize) initialize();
        states_.push_back(state);
        return std::make_unique<Lease>(this, state);
    }
    int clean(const std::function<void(const std::string&)>& mark) {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_ = epoch_ + "-" + std::to_string(++sequence_); cleaned_ = true;
        int affected = 0;
        for (const auto& state : states_) {
            if (state->cancelled.load()) continue;
            mark(state->sid); // Publish the reason before disconnecting the browser.
            state->cancelled.store(true); ++affected;
            if (state->fd != invalid_socket) {
#if defined(_WIN32)
                ::shutdown(state->fd, SD_BOTH);
#else
                ::shutdown(state->fd, SHUT_RDWR);
#endif
            }
        }
        return affected;
    }
    Counts counts() const { std::lock_guard<std::mutex> lock(mutex_); int n = static_cast<int>(states_.size()); return {n, max_, max_ - n}; }
    int active() const { return counts().active; }
    std::string generation() const { std::lock_guard<std::mutex> lock(mutex_); return generation_; }
    int cleaning() const { std::lock_guard<std::mutex> lock(mutex_); return static_cast<int>(std::count_if(states_.begin(), states_.end(), [](const auto& s) { return s->cancelled.load(); })); }
private:
    void release(const std::shared_ptr<State>& state) {
        size_t remaining;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            states_.erase(std::remove(states_.begin(), states_.end(), state), states_.end());
            remaining = states_.size();
        }
        try { log_line("INFO", "sid=" + state->sid + " 会话名额已归还，当前占用=" + std::to_string(remaining) + "/" + std::to_string(max_)); } catch (...) {}
    }
    const int max_;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<State>> states_;
    const std::string epoch_{std::to_string(std::chrono::system_clock::now().time_since_epoch().count())};
    std::string generation_{epoch_ + "-0"};
    unsigned long long sequence_{0};
    bool cleaned_{false};
};

std::string session_counts_json(const SessionRegistry& registry) {
    const auto counts = registry.counts();
    return "\"activeSessions\":" + std::to_string(counts.active) + ",\"maxSessions\":" + std::to_string(counts.maximum) +
        ",\"availableSessions\":" + std::to_string(counts.available);
}

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
        if (value.end_reason == "manual_cleanup") return;
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

struct HttpRequest { std::string method; std::string path; Query query; Query headers; };

HttpRequest read_http_request(socket_handle fd) {
    std::string raw;
    char buffer[4096];
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < 16384) {
        if (!g_running.load()) throw ClientDisconnected();
        if (std::chrono::steady_clock::now() >= deadline) throw BridgeError(400, "REQUEST_TIMEOUT", "HTTP 请求头读取超时");
        platform_pollfd request_poll{fd, static_cast<short>(platform_pollin), 0};
        if (poll_socket(&request_poll, 1, 100) == 0) continue;
        const int count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0 && socket_would_block(socket_last_error())) continue;
#if !defined(_WIN32)
        if (count < 0 && errno == EINTR) continue;
#endif
        if (count <= 0) throw ClientDisconnected();
        raw.append(buffer, static_cast<size_t>(count));
    }
    const auto line_end = raw.find("\r\n");
    if (line_end == std::string::npos) throw BridgeError(400, "BAD_REQUEST", "HTTP 请求无效");
    std::istringstream line(raw.substr(0, line_end));
    std::string target, version;
    HttpRequest request;
    if (!(line >> request.method >> target >> version)) throw BridgeError(400, "BAD_REQUEST", "HTTP 请求行无效");
    std::istringstream headers(raw.substr(line_end + 2));
    std::string header;
    while (std::getline(headers, header) && header != "\r") {
        const auto colon = header.find(':');
        if (colon == std::string::npos) continue;
        auto name = header.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        request.headers[name] = trim(header.substr(colon + 1));
    }
    const auto query_at = target.find('?');
    request.path = target.substr(0, query_at);
    request.query = query_at == std::string::npos ? Query{} : parse_query(target.substr(query_at + 1));
    return request;
}

void send_text(socket_handle fd, const std::string& text, int stall_ms = 60000) {
    if (!write_socket_all(fd, reinterpret_cast<const uint8_t*>(text.data()), text.size(), stall_ms)) throw ClientDisconnected();
}

std::string json_escape(const std::string& value) {
    std::string result;
    for (const auto c : value) { if (c == '"' || c == '\\') result.push_back('\\'); result.push_back(c); }
    return result;
}

void send_json_body(socket_handle fd, int status, const std::string& body) {
    const std::string status_text = status == 200 ? "OK" : status == 202 ? "Accepted" : status == 403 ? "Forbidden" : status == 409 ? "Conflict" : status == 204 ? "No Content" : status == 400 ? "Bad Request" : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : status == 503 ? "Service Unavailable" : "Bad Gateway";
    send_text(fd, "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n" + body);
}

void send_json(socket_handle fd, int status, const std::string& code, const std::string& message, const std::string& sid = "") {
    const std::string body = "{\"code\":\"" + json_escape(code) + "\",\"message\":\"" + json_escape(message) + "\"" + (sid.empty() ? "" : ",\"requestId\":\"" + json_escape(sid) + "\"") + "}";
    send_json_body(fd, status, body);
}

std::string format_utc(std::time_t value) {
    std::tm utc{};
    utc_time_safe(value, utc);
    std::ostringstream text;
    text << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return text.str();
}

void send_session_status(socket_handle fd, const SessionStatus& value) {
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

void send_video_headers(socket_handle fd, const StreamRequest& request, const Config& config, const std::vector<uint8_t>& initial_mp4) {
    // 初始化段含实际轨道；仅在存在音频轨时声明 mp4a，避免 MSE codec 列表与设备流不一致。
    const std::string video_codec = avc_mse_codec(initial_mp4);
    const std::string codecs = config.enable_audio && request.speed == 1 && initial_mp4_has_audio(initial_mp4) ? video_codec + ",mp4a.40.2" : video_codec;
    const std::string playback_start = request.option == Option::Playback ? std::to_string(request.start) : "";
    const std::string playback_end = request.option == Option::Playback ? std::to_string(request.end) : "";
    send_text(fd, "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nTransfer-Encoding: chunked\r\nCache-Control: no-store, no-cache\r\nAccept-Ranges: none\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: X-Hik-Bridge-Mse-Codecs, X-Hik-Bridge-Video-Codec, X-Hik-Bridge-Playback-Start, X-Hik-Bridge-Playback-End, X-Hik-Bridge-Session-Id, X-Hik-Bridge-Session-Status-Url\r\nX-Hik-Bridge-Mse-Codecs: " + codecs + "\r\nX-Hik-Bridge-Video-Codec: h264\r\nX-Hik-Bridge-Playback-Start: " + playback_start + "\r\nX-Hik-Bridge-Playback-End: " + playback_end + "\r\nX-Hik-Bridge-Session-Id: " + request.sid + "\r\nX-Hik-Bridge-Session-Status-Url: /session-status?sid=" + request.sid + "\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n");
}

void send_chunk(socket_handle fd, const uint8_t* data, size_t size, int stall_ms = 60000) {
    std::ostringstream head; head << std::hex << size << "\r\n";
    send_text(fd, head.str(), stall_ms);
    if (size && !write_socket_all(fd, data, size, stall_ms)) throw ClientDisconnected();
    send_text(fd, "\r\n", stall_ms);
}

StreamEndReason resolve_end_reason(const HcSession* session, const FfmpegProcess* ffmpeg) {
    if (session && session->end_reason() != StreamEndReason::Active) return session->end_reason();
    if (ffmpeg && ffmpeg->input_end_reason() == StreamEndReason::SdkDataTimeout) return StreamEndReason::SdkDataTimeout;
    if (ffmpeg) return StreamEndReason::FfmpegExit;
    return StreamEndReason::StreamEnded;
}

void handle_video(socket_handle fd, const StreamRequest& request, const Config& config, const FfmpegHardwareAcceleration& hardware,
                  SessionRegistry& registry, SessionStatusRegistry& statuses, VideoCodecCache& codec_cache, bool& response_started) {
    check_video_client(fd);
    auto lease = registry.reserve(request.sid, fd, request.generation, [&] { statuses.set_active(request.sid); });
    if (!lease) throw BridgeError(503, "SESSION_LIMIT", "已达到最大并发会话数");
    log_line("INFO", "sid=" + request.sid + " 已分配会话名额，当前占用=" + std::to_string(registry.active()) + "/" + std::to_string(config.max_sessions));
    const auto check_client = [fd, &lease] { if (lease->cancelled()) throw ClientDisconnected(); check_video_client(fd); };
    std::unique_ptr<HcSession> session;
    std::unique_ptr<FfmpegProcess> ffmpeg;
    std::unique_ptr<FfmpegProcess> probe_ffmpeg;
    // Declared after the resources, before the lease is destroyed: capacity is returned last.
    struct Cleanup {
        std::unique_ptr<FfmpegProcess>& probe;
        std::unique_ptr<FfmpegProcess>& output;
        std::unique_ptr<HcSession>& sdk;
        const std::string& sid;
        ~Cleanup() {
            const auto started = std::chrono::steady_clock::now();
            probe.reset(); output.reset(); sdk.reset();
            try { log_line("INFO", "sid=" + sid + " 管线资源清理完成，耗时=" +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()) + "ms。"); } catch (...) {}
        }
    } cleanup{probe_ffmpeg, ffmpeg, session, request.sid};
    bool status_published = false;
    const auto publish_status = [&](StreamEndReason reason) {
        if (status_published) return;
        if (lease->cancelled()) reason = StreamEndReason::ManualCleanup;
        statuses.set_ended(request.sid, reason, end_reason_message(reason),
            session ? session->queue_current_bytes() : 0,
            session ? session->queue_peak_bytes() : 0,
            session ? session->queue_backpressure_events() : 0);
        status_published = true;
    };

    try {
        // 先登录、解析真实通道并读取设备编码配置。支持该接口的设备能直接选择正式管线，
        // H.265 不再经历“探测会话释放→重新登录→正式取流”的双会话流程。
        session = std::make_unique<HcSession>(request, config, check_client);
        session->prepare();
        DetectedVideoCodec detected_codec = DetectedVideoCodec::Unknown;
        std::vector<uint8_t> prefix;
        bool restart_session_after_probe = false;
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
            probe_ffmpeg = std::make_unique<FfmpegProcess>(config, request, VideoOutputMode::Copy, hardware, false, check_client);
            probe_ffmpeg->start_pump(session->queue());
            auto probe_prefix = probe_ffmpeg->read_initial(config.first_media_ms);
            detected_codec = detect_video_codec(probe_prefix);
            const char* detected_text = video_codec_name(detected_codec);
            log_line("INFO", "sid=" + request.sid + " 输入视频编码探测完成：编码=" + detected_text + "。");
            codec_cache.set(request, detected_codec);

            if (detected_codec == DetectedVideoCodec::H264 && h264_copy_allowed(request)) {
                prefix = std::move(probe_prefix);
                ffmpeg = std::move(probe_ffmpeg);
                log_line("INFO", "sid=" + request.sid + " 复用 H.264 编码探测会话，避免重复登录和取流。");
            } else if (detected_codec == DetectedVideoCodec::H264) {
                // 正式管线需要转码；探测进程已经消费 PS 系统头，创建转码进程前重新取流。
                restart_session_after_probe = true;
            }
        }

        // Windows 的 1 倍速 H.264 以及 Linux 的 H.264 实时预览可零转码；Linux H.264 回放固定转码。
        // 所有快放/慢放仍必须通过 setpts 重写 fMP4 PTS。
        const bool copy_h264 = detected_codec == DetectedVideoCodec::H264 && h264_copy_allowed(request);
        auto output_mode = copy_h264 ? VideoOutputMode::Copy : VideoOutputMode::H264Transcode;
        log_line("INFO", "sid=" + request.sid + " 编码处理决策：输出=" + std::string(output_mode == VideoOutputMode::Copy ? "H.264 直通" : "转码为 H.264") + "。");
        if (detected_codec == DetectedVideoCodec::H264 && linux_h264_playback_transcode_policy(request))
            log_line("INFO", "sid=" + request.sid + " Linux H.264 回放兼容策略已启用：禁用直通并使用 libx264 重建时间戳、关键帧和参数集。");
        // H.265/未知编码不能复用 copy 探测进程，但继续复用同一 HCNetSDK 登录、实时句柄和媒体队列。
        if (probe_ffmpeg) {
            probe_ffmpeg->stop_for_pipeline_switch();
            probe_ffmpeg.reset();
            log_line("INFO", "sid=" + request.sid + " 编码探测 FFmpeg 已停止，复用同一 HCNetSDK 取流会话创建正式转码管线。");
        }
        if (restart_session_after_probe) {
            session.reset();
            session = std::make_unique<HcSession>(request, config, check_client);
            session->prepare();
            log_line("INFO", "sid=" + request.sid + " Linux H.264 回放探测后已重新建立 HCNetSDK 会话，确保正式转码从完整 PS 头开始。");
        }
        if (!session) {
            session = std::make_unique<HcSession>(request, config, check_client);
            session->prepare();
        }
        if (!session->stream_started()) {
            session->start();
        }
        const auto start_pipeline = [&](VideoOutputMode mode, bool force_software) {
            // 仅 Windows 的 H.265→H.264 场景允许选择显卡；H.264 直通和倍速软件转码不启用硬件后端。
            const bool use_software = force_software || detected_codec != DetectedVideoCodec::H265;
            ffmpeg = std::make_unique<FfmpegProcess>(config, request, mode, hardware, use_software, check_client);
            ffmpeg->start_pump(session->queue());
            // 浏览器通常会在数秒内判定首帧超时。copy 只给较短窗口，失败后为重新登录和转码预留时间。
            const int ready_timeout_ms = mode == VideoOutputMode::Copy ? std::min(config.first_media_ms, 2000) : config.first_media_ms;
            prefix = ffmpeg->read_first_media({}, ready_timeout_ms);
        };
        bool copy_fallback_required = false;
        if (ffmpeg) {
            try {
                // 编码探测只需 ftyp/moov；正式响应浏览器前必须继续等到首个媒体分片。
                prefix = ffmpeg->read_first_media(std::move(prefix), std::min(config.first_media_ms, 2000));
            } catch (const BridgeError& error) {
                if (output_mode != VideoOutputMode::Copy) throw;
                copy_fallback_required = true;
                log_line("WARN", "sid=" + request.sid + " H.264 直通未生成可播放媒体分片：错误码=" + error.code + "；将重新取流并转码为 H.264。");
            }
        } else {
            try {
                start_pipeline(output_mode, false);
            } catch (const BridgeError& error) {
                if (output_mode == VideoOutputMode::Copy) {
                    copy_fallback_required = true;
                    log_line("WARN", "sid=" + request.sid + " H.264 直通未生成可播放媒体分片：错误码=" + error.code + "；将重新取流并转码为 H.264。");
                } else {
                    if (!ffmpeg || !ffmpeg->hardware_enabled()) throw;
                    log_line("WARN", "sid=" + request.sid + " Windows 硬件转码管线启动失败，将复用 HCNetSDK 会话回退到 libx264 软件转码。");
                    ffmpeg->stop_for_pipeline_switch();
                    ffmpeg.reset();
                    start_pipeline(output_mode, true);
                }
            }
        }
        if (copy_fallback_required) {
            std::string diagnostics = ffmpeg ? trim(ffmpeg->diagnostic_tail()) : std::string{};
            if (diagnostics.size() > 600) diagnostics.erase(0, diagnostics.size() - 600);
            if (!diagnostics.empty()) log_line("WARN", "sid=" + request.sid + " H.264 直通 FFmpeg 诊断尾部：" + diagnostics);
            if (ffmpeg) {
                ffmpeg->stop_for_pipeline_switch();
                ffmpeg.reset();
            }
            // copy 进程已经消费了 PS 系统头和部分码流；重新取流，确保转码从完整 PS 头及关键帧开始。
            session.reset();
            session = std::make_unique<HcSession>(request, config, check_client);
            session->prepare();
            session->start();
            output_mode = VideoOutputMode::H264Transcode;
            start_pipeline(output_mode, true);
            log_line("INFO", "sid=" + request.sid + " H.264 直通已自动降级为 H.264 软件转码，首个媒体分片已就绪。");
        }
        // 个别旧 NVR 的压缩配置可能与实际 IP 通道码流不一致。以 fMP4 初始化段作最后校验；
        // 若“配置 H.264”实际为 H.265，继续复用当前 HCNetSDK 会话，仅替换 FFmpeg 转码管线。
        if (output_mode == VideoOutputMode::Copy && detect_video_codec(prefix) == DetectedVideoCodec::H265) {
            log_line("WARN", "sid=" + request.sid + " NVR 编码配置与实际码流不一致：配置为 H.264，实际为 H.265；将复用当前 HCNetSDK 会话切换为 H.264 转码。");
            detected_codec = DetectedVideoCodec::H265;
            codec_cache.set(request, DetectedVideoCodec::H265);
            ffmpeg->stop_for_pipeline_switch();
            ffmpeg.reset();
            output_mode = VideoOutputMode::H264Transcode;
            try {
                start_pipeline(output_mode, false);
            } catch (const BridgeError&) {
                if (!ffmpeg || !ffmpeg->hardware_enabled()) throw;
                log_line("WARN", "sid=" + request.sid + " Windows H.265 硬件转码失败，将复用 HCNetSDK 会话回退到 libx264 软件转码。");
                ffmpeg->stop_for_pipeline_switch();
                ffmpeg.reset();
                start_pipeline(output_mode, true);
            }
        }
        log_line("INFO", "sid=" + request.sid + " 视频输出已就绪，开始向浏览器传输：初始化数据=" + std::to_string(prefix.size()) + " 字节。");
        statuses.set_output_ready(request.sid);
        send_video_headers(fd, request, config, prefix);
        response_started = true;
        send_chunk(fd, prefix.data(), prefix.size(), config.client_write_stall_ms);
        uint8_t buffer[65536];
        StreamEndReason reason = StreamEndReason::Active;
        while (g_running.load()) {
            const io_size count = ffmpeg->read(buffer, sizeof(buffer));
            if (count <= 0) {
                reason = g_running.load() ? resolve_end_reason(session.get(), ffmpeg.get()) : StreamEndReason::ServerStopped;
                break;
            }
            send_chunk(fd, buffer, static_cast<size_t>(count), config.client_write_stall_ms);
        }
        if (reason == StreamEndReason::Active) reason = g_running.load() ? resolve_end_reason(session.get(), ffmpeg.get()) : StreamEndReason::ServerStopped;
        publish_status(reason);
        log_line("INFO", "sid=" + request.sid + " 视频管线结束：原因=" + std::string(end_reason_name(reason)) + "，队列当前/峰值=" + std::to_string(session->queue_current_bytes()) + "/" + std::to_string(session->queue_peak_bytes()) + " 字节，背压次数=" + std::to_string(session->queue_backpressure_events()) + "。");
        send_text(fd, "0\r\n\r\n");
    } catch (const ClientDisconnected&) {
        publish_status(g_running.load() ? StreamEndReason::ClientReplaced : StreamEndReason::ServerStopped);
        throw;
    } catch (const BridgeError& error) {
        publish_status(error.code == "FFMPEG_OUTPUT_TIMEOUT" ? StreamEndReason::FfmpegOutputTimeout :
            error.code == "CLIENT_WRITE_TIMEOUT" ? StreamEndReason::ClientWriteTimeout : StreamEndReason::StartupFailed);
        throw;
    } catch (...) {
        publish_status(g_running.load() ? StreamEndReason::StartupFailed : StreamEndReason::ServerStopped);
        throw;
    }
}

void handle_connection(socket_handle fd, const Config& config, const FfmpegHardwareAcceleration& hardware,
                       SessionRegistry& registry, SessionStatusRegistry& statuses, VideoCodecCache& codec_cache) {
    bool response_started = false;
    std::string sid;
    try {
        const auto request = read_http_request(fd);
        if (request.path == "/cleanSessions") {
            if (request.method == "OPTIONS") {
                send_text(fd, "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *" + std::string(
                    "\r\nAccess-Control-Allow-Methods: POST, OPTIONS\r\nAccess-Control-Allow-Headers: X-Hik-Cleanup\r\nCache-Control: no-store\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")); return;
            }
            if (request.method != "POST") throw BridgeError(405, "METHOD_NOT_ALLOWED", "清理会话仅支持 POST 请求");
            if (!request.query.count("apply") || request.query.at("apply") != "true")
                throw BridgeError(400, "INVALID_PARAMETER", "必须明确指定 apply=true");
            if (!request.headers.count("x-hik-cleanup") || request.headers.at("x-hik-cleanup") != "true")
                throw BridgeError(400, "INVALID_PARAMETER", "缺少 X-Hik-Cleanup: true 请求头");
            const int affected = registry.clean([&](const std::string& id) {
                statuses.set_ended(id, StreamEndReason::ManualCleanup, end_reason_message(StreamEndReason::ManualCleanup), 0, 0, 0);
            });
            const int remaining = registry.cleaning();
            send_json_body(fd, remaining ? 202 : 200, "{\"applied\":true,\"affectedSessions\":" + std::to_string(affected) +
                ",\"cleaningSessions\":" + std::to_string(remaining) + ",\"generation\":\"" + registry.generation() + "\"}");
            return;
        }
        if (request.method == "OPTIONS") { send_text(fd, "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\nContent-Length: 0\r\n\r\n"); return; }
        if (request.method != "GET") throw BridgeError(405, "METHOD_NOT_ALLOWED", "仅支持 GET 请求");
        if (request.path == "/healthz" || request.path == "/") {
            send_json_body(fd, 200, "{\"status\":\"ok\",\"sdk\":\"hcnetsdk\"," + session_counts_json(registry) + ",\"capabilities\":{\"cleanSessions\":true},\"generation\":\"" + registry.generation() + "\",\"cleaningSessions\":" + std::to_string(registry.cleaning()) + ",\"hardwareAcceleration\":\"" + hardware.backend_name() + "\"}");
            return;
        }
        if (request.path == "/version") {
            send_json_body(fd, 200, "{\"name\":\"hik-sdk-http-bridge\",\"version\":\"" HIK_BRIDGE_VERSION "\",\"releaseVersion\":\"" HIK_BRIDGE_RELEASE_VERSION "\",\"buildDate\":\"" HIK_BRIDGE_BUILD_DATE "\",\"framework\":\"C++17\",\"architecture\":\"" HIK_BRIDGE_ARCH "\",\"hardwareAcceleration\":\"" + hardware.backend_name() + "\"}");
            return;
        }
        if (request.path == "/session-status") {
            const auto sid_value = request.query.find("sid");
            if (sid_value == request.query.end() || sid_value->second.empty()) throw BridgeError(400, "INVALID_PARAMETER", "缺少必填参数 sid");
            const auto status = statuses.get(sid_value->second);
            if (!status) { send_json(fd, 404, "SESSION_NOT_FOUND", "会话状态不可用", sid_value->second); return; }
            send_session_status(fd, *status);
            return;
        }
        if (request.path == "/session-rendered") {
            const auto sid_value = request.query.find("sid");
            if (sid_value == request.query.end() || sid_value->second.empty()) throw BridgeError(400, "INVALID_PARAMETER", "缺少必填参数 sid");
            long long client_elapsed_ms = 0;
            const auto elapsed_value = request.query.find("elapsedMs");
            try { if (elapsed_value != request.query.end()) client_elapsed_ms = std::stoll(elapsed_value->second); }
            catch (...) { throw BridgeError(400, "INVALID_PARAMETER", "elapsedMs 无效"); }
            if (client_elapsed_ms < 0 || client_elapsed_ms > 600000) throw BridgeError(400, "INVALID_PARAMETER", "elapsedMs 无效");
            const auto timing = statuses.set_browser_first_frame(sid_value->second, client_elapsed_ms);
            if (!timing) { send_json(fd, 404, "SESSION_NOT_FOUND", "会话状态不可用", sid_value->second); return; }
            log_line("INFO", "sid=" + sid_value->second + " 浏览器首帧已渲染：服务端总耗时=" + std::to_string(timing->first) + "ms，视频输出至渲染=" + std::to_string(timing->second) + "ms，浏览器上报=" + std::to_string(client_elapsed_ms) + "ms。");
            send_json_body(fd, 200, "{\"status\":\"ok\"}");
            return;
        }
        if (request.path != "/video") throw BridgeError(404, "NOT_FOUND", "接口不存在");
        const auto stream_request = parse_stream_request(request.query, config);
        sid = stream_request.sid;
        log_line("INFO", "sid=" + sid + " 收到视频播放请求。");
        handle_video(fd, stream_request, config, hardware, registry, statuses, codec_cache, response_started);
    } catch (const ClientDisconnected&) {
        log_line("INFO", "sid=" + sid + " 浏览器已主动停止或替换视频流，开始释放资源。");
    } catch (const BridgeError& error) {
        log_line("WARN", "sid=" + sid + " 请求失败：错误码=" + error.code + "，HTTP 状态=" + std::to_string(error.http_status) + "。");
        if (!response_started) {
            try {
                if (error.code == "SESSION_LIMIT") send_json_body(fd, 503, "{\"code\":\"SESSION_LIMIT\",\"message\":\"已达到最大并发会话数\",\"requestId\":\"" + json_escape(sid) + "\"," + session_counts_json(registry) + "}");
                else send_json(fd, error.http_status, error.code, error.what(), sid);
            } catch (...) {}
        }
    } catch (const std::exception&) {
        log_line("ERROR", std::string("sid=") + sid + " 出现未处理异常。");
        if (!response_started) { try { send_json(fd, 500, "INTERNAL_ERROR", "服务内部错误", sid); } catch (...) {} }
    }
}

socket_handle create_listener(const Config& config) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* addresses = nullptr;
    if (::getaddrinfo(config.bind.c_str(), std::to_string(config.port).c_str(), &hints, &addresses) != 0) throw BridgeError(500, "BIND_FAILED", "监听地址无效");
    socket_handle listener = invalid_socket;
    for (auto* current = addresses; current; current = current->ai_next) {
        listener = create_socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (listener == invalid_socket) continue;
        const int on = 1;
#if defined(_WIN32)
        const int reuse_option = SO_EXCLUSIVEADDRUSE;
#else
        const int reuse_option = SO_REUSEADDR;
#endif
        if (::setsockopt(listener, SOL_SOCKET, reuse_option, reinterpret_cast<const char*>(&on), sizeof(on)) != 0) {
            close_socket(listener); listener = invalid_socket; continue;
        }
        if (::bind(listener, current->ai_addr, current->ai_addrlen) == 0 && ::listen(listener, 32) == 0) break;
        close_socket(listener); listener = invalid_socket;
    }
    ::freeaddrinfo(addresses);
    if (listener == invalid_socket) throw BridgeError(500, "BIND_FAILED", "无法绑定 HTTP 监听端口");
    return listener;
}

bool is_local_ipv4_client(const sockaddr_storage& peer) {
    if (peer.ss_family != AF_INET) return false;
    const auto* address = reinterpret_cast<const sockaddr_in*>(&peer);
    return ntohl(address->sin_addr.s_addr) == 0x7f000001U;
}

// Own all HTTP workers, including clients that never finish their headers.
// Destruction happens before SDK/configuration objects leave run_server's scope.
class ConnectionWorkers {
    struct Worker {
        socket_handle socket;
        std::thread thread;
        std::atomic_bool done{false};
        explicit Worker(socket_handle value) : socket(value) {}
    };
    std::vector<std::unique_ptr<Worker>> workers_;
    std::mutex sockets_mutex_;
    size_t maximum_;
public:
    explicit ConnectionWorkers(size_t maximum = 64) : maximum_(maximum) {}
    ~ConnectionWorkers() {
        g_running.store(false);
        {
            std::lock_guard<std::mutex> lock(sockets_mutex_);
            for (const auto& worker : workers_) {
                if (worker->socket == invalid_socket) continue;
#if defined(_WIN32)
                ::shutdown(worker->socket, SD_BOTH);
#else
                ::shutdown(worker->socket, SHUT_RDWR);
#endif
            }
        }
        for (const auto& worker : workers_) if (worker->thread.joinable()) worker->thread.join();
    }
    void reap() {
        for (auto it = workers_.begin(); it != workers_.end();) {
            if ((*it)->done.load()) { (*it)->thread.join(); it = workers_.erase(it); }
            else ++it;
        }
    }
    template<class Handler>
    bool start(socket_handle client, Handler&& handler) {
        // Configure before a best-effort busy response, so the accept loop never blocks on send.
#if defined(_WIN32)
        u_long nonblocking = 1;
        const bool configured = ::ioctlsocket(client, FIONBIO, &nonblocking) == 0;
#else
        const int flags = ::fcntl(client, F_GETFL, 0);
        const bool configured = flags >= 0 && ::fcntl(client, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
        if (!configured) { close_socket(client); return false; }
        reap();
        if (workers_.size() >= maximum_) {
            const char response[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nAccess-Control-Allow-Origin: *\r\nRetry-After: 1\r\nConnection: close\r\n\r\n";
            ::send(client, response, sizeof(response) - 1, 0);
            close_socket(client);
            return false;
        }
        try { workers_.push_back(std::make_unique<Worker>(client)); }
        catch (...) { close_socket(client); return false; }
        Worker* worker = workers_.back().get();
        try {
            worker->thread = std::thread([this, worker, handler = std::forward<Handler>(handler)]() mutable {
                try { handler(); }
                catch (...) { try { log_line("ERROR", "HTTP worker failed"); } catch (...) {} }
                {
                    std::lock_guard<std::mutex> lock(sockets_mutex_);
                    close_socket(worker->socket);
                    worker->socket = invalid_socket;
                }
                worker->done.store(true);
            });
            return true;
        } catch (...) {
            close_socket(client); workers_.pop_back();
            try { log_line("WARN", "HTTP 工作线程创建失败，已拒绝该连接，服务继续运行。"); } catch (...) {}
            return false;
        }
    }
};

void run_server(const Config& config) {
    NetworkRuntime network;
    (void)network;
    HcNetRuntime sdk(config);
#if defined(_WIN32)
    windows_service_start_progress();
#endif
    const auto hardware = FfmpegHardwareAcceleration::detect(config);
    SessionRegistry registry(config.max_sessions);
    SessionStatusRegistry statuses;
    VideoCodecCache codec_cache(config.codec_cache_seconds, config.codec_cache_file);
    const socket_handle listener = create_listener(config);
    struct ListenerGuard { socket_handle fd; ~ListenerGuard() { close_socket(fd); } } listener_guard{listener};
    ConnectionWorkers workers(static_cast<size_t>(config.max_http_connections));
    log_line("INFO", "HTTP 视频桥接服务已启动：地址=http://" + config.bind + ":" + std::to_string(config.port) + "，平台=" HIK_BRIDGE_ARCH "，转码后端=" + hardware.backend_name() + "。");
#if defined(_WIN32)
    windows_service_ready();
#endif
    while (g_running.load()) {
        workers.reap();
        platform_pollfd listener_poll{listener, static_cast<short>(platform_pollin), 0};
        const int poll_result = poll_socket(&listener_poll, 1, 500);
        if (poll_result < 0) {
#if !defined(_WIN32)
            if (errno == EINTR) continue;
#endif
            log_line("WARN", "等待 HTTP 连接失败：系统错误=" + std::to_string(socket_last_error()) + "。");
            continue;
        }
        if (poll_result == 0) continue;
        sockaddr_storage peer{};
        socket_length length = sizeof(peer);
        const socket_handle client = accept_client(listener, reinterpret_cast<sockaddr*>(&peer), &length);
        if (client == invalid_socket) { log_line("WARN", "接受 HTTP 连接失败：系统错误=" + std::to_string(socket_last_error()) + "。"); continue; }
        if (!is_local_ipv4_client(peer)) { log_line("WARN", "已拒绝非本机 IPv4 客户端的 HTTP 请求。"); close_socket(client); continue; }
        workers.start(client, [client, &config, &hardware, &registry, &statuses, &codec_cache] { handle_connection(client, config, hardware, registry, statuses, codec_cache); });
    }
}

} // namespace

int run_bridge(int argc, char** argv, bool service_mode = false) {
    try {
#if defined(_WIN32)
        ConsoleCodePageScope console_code_pages;
        if (!service_mode) { SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8); }
#else
        (void)service_mode;
        std::setlocale(LC_ALL, "");
#endif
        if (argc > 1 && (std::string(argv[1]) == "version" || std::string(argv[1]) == "--version")) { std::cout << "hik-sdk-http-bridge " HIK_BRIDGE_VERSION "\n"; return 0; }
#if defined(_WIN32)
        wchar_t module[MAX_PATH]{};
        fs::path config_path{"config.json"};
        if (GetModuleFileNameW(nullptr, module, MAX_PATH)) config_path = fs::path(module).parent_path() / "config.json";
        // SCM otherwise starts in System32. Match the portable CMD launcher's
        // directory for vendor components, while resolving config paths below.
        if (service_mode) fs::current_path(config_path.parent_path());
#else
        fs::path config_path{"/opt/hik-bridge/config/config.json"};
#endif
        bool validate_config_only = false;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "run") continue;
#if defined(_WIN32)
            if (argument == "service" && service_mode) continue;
#endif
            if (argument == "validate-config" || argument == "--validate-config") { validate_config_only = true; continue; }
            if (argument == "--config") {
                if (++index >= argc) throw BridgeError(400, "INVALID_COMMAND", "");
                config_path = fs::u8path(argv[index]);
                continue;
            }
            throw BridgeError(400, "INVALID_COMMAND", "");
        }
        const Config config = load_config(config_path);
#if defined(_WIN32)
        if (!fs::exists(config.sdk_directory / "HCNetSDK.dll")) throw BridgeError(500, "SDK_NOT_FOUND", "");
#else
        if (!fs::exists(config.sdk_directory / "libhcnetsdk.so")) throw BridgeError(500, "SDK_NOT_FOUND", "");
#endif
        if (!fs::exists(config.ffmpeg_path)) throw BridgeError(500, "FFMPEG_NOT_FOUND", "");
        if (validate_config_only) { write_stdout_line("配置校验通过：" + fs::absolute(config_path).u8string()); return 0; }
        g_daily_logger = std::make_unique<DailyLogWriter>(config.log_directory, config.log_retention_days);
        if (!service_mode) {
            ::signal(SIGINT, on_signal);
            ::signal(SIGTERM, on_signal);
        }
#if !defined(_WIN32)
        ::signal(SIGPIPE, SIG_IGN);
#endif
        run_server(config);
        return 0;
    } catch (const BridgeError& error) {
        const bool config_error = error.code == "CONFIG_NOT_FOUND" || error.code == "INVALID_CONFIG" || error.code == "INVALID_COMMAND" || error.code == "SDK_NOT_FOUND" || error.code == "FFMPEG_NOT_FOUND";
        log_line("ERROR", std::string(config_error ? "配置错误：错误码=" : "服务启动失败：错误码=") + error.code + "。");
#if defined(_WIN32)
        if (service_mode) windows_service_error(error.code.c_str());
#endif
        return 2;
    } catch (const std::exception&) {
        log_line("ERROR", "服务启动时发生未处理异常。");
#if defined(_WIN32)
        if (service_mode) windows_service_error("Unhandled exception during bridge startup/shutdown");
#endif
        return 1;
    }
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    // Preserve Unicode paths passed by SCM and PowerShell independently of ACP.
    std::vector<std::string> arguments;
    std::vector<char*> pointers;
    for (int i = 0; i < argc; ++i) arguments.push_back(fs::path(argv[i]).u8string());
    for (auto& argument : arguments) pointers.push_back(argument.data());
    if (argc > 1 && arguments[1] == "service")
        return run_windows_service([&] { return run_bridge(argc, pointers.data(), true); }, [] { g_running.store(false); });
    return run_bridge(argc, pointers.data());
}
#else
int main(int argc, char** argv) { return run_bridge(argc, argv); }
#endif
