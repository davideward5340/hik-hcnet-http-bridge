using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using HikSdkHttpBridge.Config;

namespace HikSdkHttpBridge.Media
{
    internal enum HardwareAccelerationBackend
    {
        None,
        NvidiaNvenc,
        IntelQsv,
        AmdAmf
    }

    // 仅在 FFmpeg 已编译相应编码器、当前 Windows 驱动也能完成一次真实 H.264 编码时才启用。
    // 这样可避免把“FFmpeg 列出了编码器”误判成“客户机显卡可用”。
    internal sealed class FfmpegHardwareAcceleration
    {
        private readonly HardwareAccelerationBackend _backend;
        private readonly bool _nvidiaH264Decoder;
        private readonly bool _nvidiaHevcDecoder;

        private FfmpegHardwareAcceleration(HardwareAccelerationBackend backend, bool nvidiaH264Decoder, bool nvidiaHevcDecoder)
        {
            _backend = backend;
            _nvidiaH264Decoder = nvidiaH264Decoder;
            _nvidiaHevcDecoder = nvidiaHevcDecoder;
        }

        public static FfmpegHardwareAcceleration Disabled { get { return new FfmpegHardwareAcceleration(HardwareAccelerationBackend.None, false, false); } }
        public bool IsAvailable { get { return _backend != HardwareAccelerationBackend.None; } }
        public string BackendName
        {
            get
            {
                switch (_backend)
                {
                    case HardwareAccelerationBackend.NvidiaNvenc: return "nvidia-nvenc";
                    case HardwareAccelerationBackend.IntelQsv: return "intel-qsv";
                    case HardwareAccelerationBackend.AmdAmf: return "amd-amf";
                    default: return "software";
                }
            }
        }

        public static FfmpegHardwareAcceleration Detect(AppConfig config)
        {
            var requested = Normalize(config.Ffmpeg.HardwareAcceleration);
            if (requested == "off")
            {
                Log.Info("配置已关闭 FFmpeg 硬件转码，将使用软件转码。");
                return Disabled;
            }

            var timeoutMs = config.Ffmpeg.HardwareProbeTimeoutMs;
            var hwaccels = Run(config.Ffmpeg.Path, "-hide_banner -hwaccels", timeoutMs);
            var encoders = Run(config.Ffmpeg.Path, "-hide_banner -encoders", timeoutMs);
            var decoders = Run(config.Ffmpeg.Path, "-hide_banner -decoders", timeoutMs);
            if (hwaccels.TimedOut || encoders.TimedOut)
            {
                Log.Warn("FFmpeg 硬件能力探测超时，将使用 H.264 软件转码。");
                return Disabled;
            }

            var capabilities = new FfmpegCapabilities(hwaccels.Output, encoders.Output, decoders.Output);
            foreach (var candidate in Candidates(requested))
            {
                if (!capabilities.HasEncoder(candidate.Encoder) || !capabilities.HasHardwareMethod(candidate.Method))
                {
                    continue;
                }

                var test = Run(config.Ffmpeg.Path, "-hide_banner -loglevel error -f lavfi -i color=c=black:s=64x64:r=1 -frames:v 1 -an -c:v " + candidate.Encoder + " -f null -", timeoutMs);
                if (test.ExitCode == 0 && !test.TimedOut)
                {
                    var h264Cuvid = candidate.Backend == HardwareAccelerationBackend.NvidiaNvenc && capabilities.HasDecoder("h264_cuvid");
                    var hevcCuvid = candidate.Backend == HardwareAccelerationBackend.NvidiaNvenc && capabilities.HasDecoder("hevc_cuvid");
                    Log.Info("已启用 FFmpeg 硬件转码：后端=" + candidate.Name + "，编码器=" + candidate.Encoder + "，H.264 硬解=" + h264Cuvid + "，H.265 硬解=" + hevcCuvid + "。");
                    return new FfmpegHardwareAcceleration(candidate.Backend, h264Cuvid, hevcCuvid);
                }

            }

            Log.Warn("未检测到可用的 FFmpeg 硬件编码器，将使用 H.264 软件转码。");
            return Disabled;
        }

        public FfmpegTranscodePlan SelectPlan(VideoOutputMode outputMode, DetectedVideoCodec inputCodec)
        {
            if (outputMode == VideoOutputMode.Copy || !IsAvailable) return FfmpegTranscodePlan.Software;
            switch (_backend)
            {
                case HardwareAccelerationBackend.NvidiaNvenc:
                    var decoder = inputCodec == DetectedVideoCodec.H265 && _nvidiaHevcDecoder ? "hevc_cuvid" :
                        inputCodec == DetectedVideoCodec.H264 && _nvidiaH264Decoder ? "h264_cuvid" : null;
                    return new FfmpegTranscodePlan(true, BackendName, "h264_nvenc", decoder);
                case HardwareAccelerationBackend.IntelQsv:
                    // QSV/AMF 均可接收软件解码后的帧并在 GPU 编码；这样避免个别显卡的硬解初始化失败。
                    return new FfmpegTranscodePlan(true, BackendName, "h264_qsv", null);
                case HardwareAccelerationBackend.AmdAmf:
                    return new FfmpegTranscodePlan(true, BackendName, "h264_amf", null);
                default:
                    return FfmpegTranscodePlan.Software;
            }
        }

        private static Candidate[] Candidates(string requested)
        {
            var nvidia = new Candidate(HardwareAccelerationBackend.NvidiaNvenc, "nvidia-nvenc", "cuda", "h264_nvenc");
            var qsv = new Candidate(HardwareAccelerationBackend.IntelQsv, "intel-qsv", "qsv", "h264_qsv");
            var amf = new Candidate(HardwareAccelerationBackend.AmdAmf, "amd-amf", "amf", "h264_amf");
            switch (requested)
            {
                case "nvidia": return new[] { nvidia };
                case "qsv": return new[] { qsv };
                case "amf": return new[] { amf };
                default: return new[] { nvidia, qsv, amf };
            }
        }

        internal static string Normalize(string value)
        {
            var normalized = (value ?? "auto").Trim().ToLowerInvariant();
            if (normalized == "none" || normalized == "disabled" || normalized == "software") return "off";
            if (normalized == "nvenc" || normalized == "cuda") return "nvidia";
            if (normalized == "intel") return "qsv";
            if (normalized == "amd") return "amf";
            return normalized;
        }

        private static FfmpegCommandResult Run(string executable, string arguments, int timeoutMs)
        {
            try
            {
                using (var process = new Process())
                {
                    process.StartInfo = new ProcessStartInfo
                    {
                        FileName = executable,
                        Arguments = arguments,
                        UseShellExecute = false,
                        CreateNoWindow = true,
                        RedirectStandardOutput = true,
                        RedirectStandardError = true
                    };
                    process.Start();
                    var stdout = process.StandardOutput.ReadToEndAsync();
                    var stderr = process.StandardError.ReadToEndAsync();
                    if (!process.WaitForExit(timeoutMs))
                    {
                        try { process.Kill(); } catch { }
                        try { process.WaitForExit(); } catch { }
                        return new FfmpegCommandResult(-1, true, ReadTask(stdout) + Environment.NewLine + ReadTask(stderr));
                    }
                    return new FfmpegCommandResult(process.ExitCode, false, ReadTask(stdout) + Environment.NewLine + ReadTask(stderr));
                }
            }
            catch (Exception ex)
            {
                return new FfmpegCommandResult(-1, false, ex.Message);
            }
        }

        private static string ReadTask(System.Threading.Tasks.Task<string> task)
        {
            try { return task.GetAwaiter().GetResult(); } catch { return string.Empty; }
        }

        private static string TrimForLog(string value)
        {
            var text = (value ?? string.Empty).Replace('\r', ' ').Replace('\n', ' ').Trim();
            return text.Length <= 500 ? text : text.Substring(text.Length - 500);
        }

        private sealed class Candidate
        {
            public Candidate(HardwareAccelerationBackend backend, string name, string method, string encoder) { Backend = backend; Name = name; Method = method; Encoder = encoder; }
            public HardwareAccelerationBackend Backend { get; private set; }
            public string Name { get; private set; }
            public string Method { get; private set; }
            public string Encoder { get; private set; }
        }

        private sealed class FfmpegCapabilities
        {
            private readonly string _hardwareMethods;
            private readonly string _encoders;
            private readonly string _decoders;
            public FfmpegCapabilities(string hardwareMethods, string encoders, string decoders) { _hardwareMethods = hardwareMethods ?? string.Empty; _encoders = encoders ?? string.Empty; _decoders = decoders ?? string.Empty; }
            public string HardwareMethods { get { return Summary(_hardwareMethods, new[] { "cuda", "qsv", "amf" }); } }
            public string HardwareEncoders { get { return Summary(_encoders, new[] { "h264_nvenc", "h264_qsv", "h264_amf" }); } }
            public bool HasHardwareMethod(string name) { return HasLineToken(_hardwareMethods, name); }
            public bool HasEncoder(string name) { return HasLineToken(_encoders, name); }
            public bool HasDecoder(string name) { return HasLineToken(_decoders, name); }
            private static string Summary(string text, string[] names)
            {
                var result = new StringBuilder();
                foreach (var name in names)
                {
                    if (!HasLineToken(text, name)) continue;
                    if (result.Length > 0) result.Append(',');
                    result.Append(name);
                }
                return result.Length == 0 ? "none" : result.ToString();
            }
            private static bool HasLineToken(string text, string token)
            {
                using (var reader = new StringReader(text ?? string.Empty))
                {
                    string line;
                    while ((line = reader.ReadLine()) != null)
                    {
                        var fields = line.Trim().Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
                        foreach (var field in fields) if (string.Equals(field, token, StringComparison.OrdinalIgnoreCase)) return true;
                    }
                }
                return false;
            }
        }

        private sealed class FfmpegCommandResult
        {
            public FfmpegCommandResult(int exitCode, bool timedOut, string output) { ExitCode = exitCode; TimedOut = timedOut; Output = output ?? string.Empty; }
            public int ExitCode { get; private set; }
            public bool TimedOut { get; private set; }
            public string Output { get; private set; }
        }
    }

    internal sealed class FfmpegTranscodePlan
    {
        public static readonly FfmpegTranscodePlan Software = new FfmpegTranscodePlan(false, "software", "", null);
        public FfmpegTranscodePlan(bool hardwareEnabled, string backendName, string videoEncoder, string inputDecoder)
        {
            HardwareEnabled = hardwareEnabled;
            BackendName = backendName;
            VideoEncoder = videoEncoder;
            InputDecoder = inputDecoder;
        }
        public bool HardwareEnabled { get; private set; }
        public string BackendName { get; private set; }
        public string VideoEncoder { get; private set; }
        public string InputDecoder { get; private set; }
    }
}
