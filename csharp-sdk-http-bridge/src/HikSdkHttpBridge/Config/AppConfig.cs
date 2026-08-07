using System;
using System.IO;
using System.Web.Script.Serialization;

namespace HikSdkHttpBridge.Config
{
    internal sealed class AppConfig
    {
        public string SdkType { get; set; } = "hcnetsdk";
        public ServerConfig Server { get; set; } = new ServerConfig();
        public SdkConfig Sdk { get; set; } = new SdkConfig();
        public FfmpegConfig Ffmpeg { get; set; } = new FfmpegConfig();
        public MediaConfig Media { get; set; } = new MediaConfig();
        public TimeoutConfig Timeouts { get; set; } = new TimeoutConfig();
        public LoggingConfig Logging { get; set; } = new LoggingConfig();
        public DevelopmentConfig Development { get; set; } = new DevelopmentConfig();

        public static AppConfig Load(string path)
        {
            if (!File.Exists(path)) throw new ConfigException("CONFIG_NOT_FOUND", "configuration file not found");
            try
            {
                var json = File.ReadAllText(path);
                var config = new JavaScriptSerializer().Deserialize<AppConfig>(json) ?? new AppConfig();
                config.Validate();
                return config;
            }
            catch (ConfigException) { throw; }
            catch (Exception ex) { throw new ConfigException("INVALID_CONFIG", ex.Message); }
        }

        public void ResolvePaths(string baseDirectory)
        {
            Sdk.Directory = Resolve(baseDirectory, Sdk.Directory);
            Sdk.HcnetDirectory = Resolve(baseDirectory, Sdk.HcnetDirectory);
            Ffmpeg.Path = Resolve(baseDirectory, Ffmpeg.Path);
            Media.CodecCacheFile = Resolve(baseDirectory, Media.CodecCacheFile);
            Logging.Directory = Resolve(baseDirectory, Logging.Directory);
        }

        private static string Resolve(string root, string value)
        {
            return Path.GetFullPath(Path.IsPathRooted(value) ? value : Path.Combine(root, value));
        }

        private void Validate()
        {
            if (!string.Equals(SdkType, "hcnetsdk", StringComparison.OrdinalIgnoreCase)) throw new ConfigException("INVALID_CONFIG", "sdktype must be hcnetsdk");
            // 桥接服务会接收 NVR 凭据，禁止绑定到局域网、公网或 IPv6 回环地址。
            if (Server == null || !string.Equals(Server.Bind, "127.0.0.1", StringComparison.Ordinal) || Server.Port < 1 || Server.Port > 65535 || Server.Threads < 1 || Server.MaxSessions < 1) throw new ConfigException("INVALID_CONFIG", "server.bind must be 127.0.0.1 and server settings must be valid");
            if (Sdk == null || string.IsNullOrWhiteSpace(Sdk.HcnetDirectory) || Sdk.RealPlayKeyFrameIntervalFrames < 0 || Sdk.RealPlayKeyFrameIntervalFrames > ushort.MaxValue || Sdk.ConnectProbeTimeoutMs < 100 || Sdk.ConnectProbeTimeoutMs > 10000) throw new ConfigException("INVALID_CONFIG", "invalid sdk settings");
            if (Ffmpeg == null || string.IsNullOrWhiteSpace(Ffmpeg.Path)) throw new ConfigException("INVALID_CONFIG", "ffmpeg.path is required");
            var hardwareAcceleration = (Ffmpeg.HardwareAcceleration ?? "auto").Trim().ToLowerInvariant();
            if (hardwareAcceleration != "auto" && hardwareAcceleration != "off" && hardwareAcceleration != "none" && hardwareAcceleration != "disabled" && hardwareAcceleration != "software" &&
                hardwareAcceleration != "nvidia" && hardwareAcceleration != "nvenc" && hardwareAcceleration != "cuda" && hardwareAcceleration != "qsv" && hardwareAcceleration != "intel" && hardwareAcceleration != "amf" && hardwareAcceleration != "amd")
                throw new ConfigException("INVALID_CONFIG", "ffmpeg.hardwareAcceleration must be auto, off, nvidia, qsv, or amf");
            if (Ffmpeg.HardwareProbeTimeoutMs < 1000 || Ffmpeg.HardwareProbeTimeoutMs > 30000)
                throw new ConfigException("INVALID_CONFIG", "ffmpeg.hardwareProbeTimeoutMs must be between 1000 and 30000");
            if (Media == null || Media.QueueBytes < 65536 || Media.QueueBackpressureMs < 0 || Media.QueueBackpressureMs > 5000 || Media.CodecCacheSeconds < 0 || Media.CodecCacheSeconds > 86400 || Media.MaxPlaybackSeconds < 1 ||
                !string.Equals(Media.OutputVideoCodec, "h264", StringComparison.OrdinalIgnoreCase) ||
                string.IsNullOrWhiteSpace(Media.VideoEncoder) || string.IsNullOrWhiteSpace(Media.VideoPreset) || string.IsNullOrWhiteSpace(Media.CodecCacheFile))
                throw new ConfigException("INVALID_CONFIG", "invalid media settings");
            if (Timeouts == null || Timeouts.SdkStartMs < 1 || Timeouts.FirstMediaMs < 1 || Timeouts.NoSdkDataMs < 1 ||
                Timeouts.PlaybackKeepAliveMs < 1000 || Timeouts.PlaybackKeepAliveMs > 5000)
                throw new ConfigException("INVALID_CONFIG", "invalid timeout settings");
        }
    }

    internal sealed class ServerConfig { public string Bind { get; set; } = "127.0.0.1"; public int Port { get; set; } = 18080; public int Threads { get; set; } = 8; public int MaxSessions { get; set; } = 8; }
    internal sealed class SdkConfig
    {
        public string Directory { get; set; } = "./sdk/bin";
        public string HcnetDirectory { get; set; } = "./hcnetsdk";
        // 登录并解析真实通道后先读取设备压缩参数；支持时可直接选择 H.264 直通或 H.265 转码，避免先取流探测再重建会话。
        public bool ReadVideoCodecFromDevice { get; set; } = true;
        // 实时取流成功后尝试请求设备立刻发送 I 帧。设备/权限不支持时仅记录日志，不影响播放。
        public bool ForceKeyFrameOnRealPlay { get; set; } = true;
        // 目标 I 帧间隔（帧数）。25 约等于 25fps 设备的一秒；0 表示不修改 NVR 的持久编码配置。
        public int RealPlayKeyFrameIntervalFrames { get; set; } = 25;
        // 在 HCNetSDK 登录前测试 NVR SDK TCP 端口，快速区分设备不可达、端口错误和后续认证错误。
        public int ConnectProbeTimeoutMs { get; set; } = 1500;
    }
    internal sealed class FfmpegConfig
    {
        public string Path { get; set; } = "./ffmpeg/ffmpeg.exe";
        public string LogLevel { get; set; } = "info";
        // auto 依次尝试 NVIDIA NVENC、Intel QSV、AMD AMF；失败后安全回退到 libx264。
        public string HardwareAcceleration { get; set; } = "auto";
        // 单个硬件能力/真实编码探测的最长等待时间，避免显卡驱动异常阻塞服务启动。
        public int HardwareProbeTimeoutMs { get; set; } = 3000;
    }
    internal sealed class MediaConfig
    {
        // 回放中 FFmpeg、HTTP 或 MSE 的短暂抖动由此队列吸收。容量仍受 server.maxSessions 限制，
        // 避免将其设置为无上限而耗尽桥接机内存。
        public long QueueBytes { get; set; } = 67108864;
        // SDK 回调遇到短暂拥塞时最多等待该时长；超过后明确以 queue_overflow 结束会话，由前端重建。
        public int QueueBackpressureMs { get; set; } = 500;
        // 浏览器只接收 AVC/H.264 fMP4；HEVC/H.265 输入由 FFmpeg 解码后转为本编码。
        public string OutputVideoCodec { get; set; } = "h264";
        public string VideoEncoder { get; set; } = "libx264";
        public string VideoPreset { get; set; } = "veryfast";
        // 默认仅输出视频，降低浏览器下行带宽和 AAC 编码/封装开销。需要声音时可显式设为 true。
        public bool EnableAudio { get; set; } = false;
        // 同一 NVR 通道的编码通常稳定；缓存可避免每次播放先额外取一次流做编码探测。0 表示禁用。
        public int CodecCacheSeconds { get; set; } = 900;
        // 仅保存设备、通道、码流、编码和过期时间，绝不保存用户名或密码；用于服务重启后的首次请求。
        public string CodecCacheFile { get; set; } = "./state/video-codec-cache.json";
        public bool DisableAudioWhenSpeedChanged { get; set; } = true;
        public long MaxPlaybackSeconds { get; set; } = 86400;
    }
    internal sealed class TimeoutConfig
    {
        public int ResolveUrlMs { get; set; } = 5000;
        public int SdkStartMs { get; set; } = 10000;
        public int FirstMediaMs { get; set; } = 15000;
        public int NoSdkDataMs { get; set; } = 10000;
        // SDK 文档建议回放数据回调可能被阻塞时约每 2 秒发送一次 KEEPALIVE；这里留出安全余量。
        public int PlaybackKeepAliveMs { get; set; } = 1500;
        public int HttpWriteMs { get; set; } = 10000;
        public int StopMs { get; set; } = 5000;
    }
    internal sealed class LoggingConfig { public string Directory { get; set; } = "./logs"; public string Level { get; set; } = "info"; public int RetentionDays { get; set; } = 30; }
    internal sealed class DevelopmentConfig { public bool AllowDirectSourceUrl { get; set; } }

    internal sealed class ConfigException : Exception
    {
        public ConfigException(string code, string message) : base(message) { Code = code; }
        public string Code { get; private set; }
    }
}
