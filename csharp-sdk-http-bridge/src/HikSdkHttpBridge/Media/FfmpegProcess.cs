using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using HikSdkHttpBridge.Config;
using HikSdkHttpBridge.Model;

namespace HikSdkHttpBridge.Media
{
    internal enum VideoOutputMode { Copy, H264Transcode }
    internal enum DetectedVideoCodec { Unknown, H264, H265 }

    internal static class VideoCodecText
    {
        public static string DisplayName(DetectedVideoCodec codec)
        {
            switch (codec)
            {
                case DetectedVideoCodec.H264: return "H.264";
                case DetectedVideoCodec.H265: return "H.265";
                default: return "未识别";
            }
        }
    }

    internal sealed class FfmpegProcess : IDisposable
    {
        private readonly Process _process;
        private readonly CancellationTokenSource _stop = new CancellationTokenSource();
        private readonly StringBuilder _errors = new StringBuilder();
        private readonly string _sid;
        private readonly Stopwatch _lifetime = Stopwatch.StartNew();
        private Task _inputTask;
        private long _inputBytes;
        private long _inputPackets;
        private string _inputVideoCodec = "待识别";
        private string _inputEndReason = "active";
        private int _inputCodecLogged;
        private int _processStopped;
        private int _disposed;
        private readonly VideoOutputMode _videoOutputMode;
        private readonly FfmpegTranscodePlan _transcodePlan;
        private readonly DetectedVideoCodec _sourceVideoCodec;
        private string _mseVideoCodec = "avc1.640029";
        private DetectedVideoCodec _detectedVideoCodec = DetectedVideoCodec.Unknown;

        public FfmpegProcess(AppConfig config, StreamRequest request, VideoOutputMode videoOutputMode, DetectedVideoCodec sourceVideoCodec, FfmpegHardwareAcceleration hardwareAcceleration)
        {
            _sid = request.Sid;
            // 变速场景的音频时间轴未重采样，始终不输出音频；正常速度是否输出由配置明确控制。
            _audioRequested = config.Media.EnableAudio && request.Speed == 1;
            _videoOutputMode = videoOutputMode;
            _sourceVideoCodec = sourceVideoCodec;
            _transcodePlan = (hardwareAcceleration ?? FfmpegHardwareAcceleration.Disabled).SelectPlan(videoOutputMode, sourceVideoCodec);
            if (!File.Exists(config.Ffmpeg.Path)) throw new BridgeException(500, "FFMPEG_NOT_FOUND", "ffmpeg executable not found");
            var args = BuildArguments(config, request, videoOutputMode, _transcodePlan);
            _process = new Process
            {
                StartInfo = new ProcessStartInfo
                {
                    FileName = config.Ffmpeg.Path,
                    Arguments = args,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardInput = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true
                },
                EnableRaisingEvents = true
            };
            _process.ErrorDataReceived += OnErrorData;
            if (!_process.Start()) throw new BridgeException(500, "FFMPEG_START_FAILED", "could not start ffmpeg");
            _process.BeginErrorReadLine();
            Log.Info("sid=" + _sid + " FFmpeg 转流进程已启动：模式=" + (videoOutputMode == VideoOutputMode.Copy ? "H.264 直通/编码探测" : "转码为 H.264") + "，输入编码=" + VideoCodecText.DisplayName(sourceVideoCodec) + "，转码后端=" + DisplayBackendName(_transcodePlan.BackendName) + "，编码器=" + (_transcodePlan.HardwareEnabled ? _transcodePlan.VideoEncoder : config.Media.VideoEncoder) + "，速度=" + request.Speed.ToString(CultureInfo.InvariantCulture) + "x，音频=" + (_audioRequested ? "启用" : "关闭") + "。");
        }

        public Stream Output { get { return _process.StandardOutput.BaseStream; } }
        public string MseCodecs { get { return _mseVideoCodec + (Volatile.Read(ref _outputHasAudio) != 0 ? ",mp4a.40.2" : ""); } }
        public DetectedVideoCodec DetectedVideoCodec { get { return _detectedVideoCodec; } }
        public string InputEndReason { get { return _inputEndReason; } }
        private readonly bool _audioRequested;
        private int _outputHasAudio;

        // fMP4 初始化段已经包含实际轨道；在发送 HTTP 头之前据此确定 MSE codecs，避免无音频设备被错误声明为 AAC。
        public void InspectInitialMp4(byte[] initialMp4)
        {
            if (_audioRequested && ContainsAscii(initialMp4, "soun")) Interlocked.Exchange(ref _outputHasAudio, 1);
            if (ContainsAscii(initialMp4, "hvcC")) _detectedVideoCodec = DetectedVideoCodec.H265;
            else if (ContainsAscii(initialMp4, "avcC")) _detectedVideoCodec = DetectedVideoCodec.H264;
            else _detectedVideoCodec = DetectedVideoCodec.Unknown;
            if (_videoOutputMode == VideoOutputMode.Copy && _detectedVideoCodec == DetectedVideoCodec.H264)
            {
                _mseVideoCodec = ReadAvcCodec(initialMp4) ?? _mseVideoCodec;
            }
        }

        public void StartInput(ByteBoundedQueue queue, int noDataTimeoutMs)
        {
            _inputTask = Task.Run(() => PumpInput(queue, noDataTimeoutMs));
        }

        private void PumpInput(ByteBoundedQueue queue, int timeoutMs)
        {
            try
            {
                var input = _process.StandardInput.BaseStream;
                while (!_stop.IsCancellationRequested)
                {
                    var data = queue.Dequeue(timeoutMs);
                    if (data == null)
                    {
                        if (queue.IsCompleted) break;
                        _inputEndReason = "sdk_data_timeout";
                        throw new IOException("HCNetSDK stopped delivering media data");
                    }
                    input.Write(data, 0, data.Length);
                    Interlocked.Increment(ref _inputPackets);
                    Interlocked.Add(ref _inputBytes, data.Length);
                }
            }
            catch (Exception) { if (!_stop.IsCancellationRequested) Log.Warn("sid=" + _sid + " FFmpeg 输入管道异常结束：原因=" + _inputEndReason + "。"); }
            finally
            {
                try { _process.StandardInput.Close(); } catch { }
            }
        }

        public string ErrorText { get { lock (_errors) return _errors.ToString(); } }
        public bool HasExited { get { try { return _process.HasExited; } catch { return true; } } }

        // H.265 兜底探测完成后，保留同一条 HCNetSDK 取流会话并立即切换 FFmpeg，
        // 避免默认 Dispose 的 1.5 秒优雅退出等待拖慢首画面。
        public void StopForPipelineSwitch()
        {
            StopProcess(true);
        }

        private void OnErrorData(object sender, DataReceivedEventArgs e)
        {
            if (string.IsNullOrEmpty(e.Data)) return;
            var line = e.Data;
            lock (_errors) { if (_errors.Length < 8192) _errors.AppendLine(line); }
            var codec = DetectInputCodec(line);
            if (_audioRequested && line.IndexOf("Audio:", StringComparison.OrdinalIgnoreCase) >= 0) Interlocked.Exchange(ref _outputHasAudio, 1);
            if (codec == null) return;
            _inputVideoCodec = codec;
            if (Interlocked.Exchange(ref _inputCodecLogged, 1) == 0)
            {
                var decision = _videoOutputMode == VideoOutputMode.Copy ? "H.264 直通" :
                    (_transcodePlan.HardwareEnabled ? "H.264 硬件转码：后端=" + _transcodePlan.BackendName + "，编码器=" + _transcodePlan.VideoEncoder : "H.264 软件转码");
                Log.Info("sid=" + _sid + " FFmpeg 已识别输入视频编码=" + codec + "，处理方式=" + decision + "。");
            }
        }

        private static string DetectInputCodec(string line)
        {
            if (line.IndexOf("Video: hevc", StringComparison.OrdinalIgnoreCase) >= 0 || line.IndexOf("Video: h265", StringComparison.OrdinalIgnoreCase) >= 0) return "H.265";
            if (line.IndexOf("Video: h264", StringComparison.OrdinalIgnoreCase) >= 0 || line.IndexOf("Video: avc", StringComparison.OrdinalIgnoreCase) >= 0) return "H.264";
            return null;
        }

        private static bool ContainsAscii(byte[] data, string text)
        {
            return FindAscii(data, text) >= 0;
        }

        private static int FindAscii(byte[] data, string text)
        {
            var target = Encoding.ASCII.GetBytes(text);
            for (var i = 0; i <= data.Length - target.Length; i++)
            {
                var matched = true;
                for (var j = 0; j < target.Length; j++) if (data[i + j] != target[j]) { matched = false; break; }
                if (matched) return i;
            }
            return -1;
        }

        private static string ReadAvcCodec(byte[] data)
        {
            var at = FindAscii(data, "avcC");
            if (at < 0 || at + 7 >= data.Length) return null;
            return "avc1." + data[at + 5].ToString("x2") + data[at + 6].ToString("x2") + data[at + 7].ToString("x2");
        }

        private static string BuildArguments(AppConfig config, StreamRequest request, VideoOutputMode videoOutputMode, FfmpegTranscodePlan transcodePlan)
        {
            var sb = new StringBuilder();
            sb.Append("-hide_banner -loglevel ").Append(config.Ffmpeg.LogLevel);
            // 设备码流经 HCNetSDK 直接送入管道；缩小探测窗口可更快拿到 fMP4 初始化段。
            if (!string.IsNullOrWhiteSpace(transcodePlan.InputDecoder))
                sb.Append(" -hwaccel cuda -hwaccel_output_format cuda -c:v ").Append(transcodePlan.InputDecoder);
            sb.Append(" -fflags +genpts+nobuffer -analyzeduration 250000 -probesize 262144 -i pipe:0 -map 0:v:0 ");
            if (videoOutputMode == VideoOutputMode.Copy)
                sb.Append("-c:v copy ");
            else
            {
                AppendVideoEncoderArguments(sb, config, transcodePlan);
                // 浏览器按 fMP4 PTS 播放；HCNetSDK 即使更快发送帧也不会改变 PTS，因此变速时必须重写时间戳。
                if (request.Speed != 1) sb.Append("-vf setpts=PTS/").Append(request.Speed.ToString("0.####", CultureInfo.InvariantCulture)).Append(' ');
            }
            if (config.Media.EnableAudio && request.Speed == 1)
                sb.Append("-map 0:a:0? -c:a aac -ar 48000 -ac 2 ");
            else
                sb.Append("-an ");
            // 250ms 分片会更快向 MSE 交付首个媒体片段；不改变视频编码和时间轴语义。
            sb.Append("-f mp4 -movflags frag_keyframe+empty_moov+default_base_moof+omit_tfhd_offset -frag_duration 250000 pipe:1");
            return sb.ToString();
        }

        private static void AppendVideoEncoderArguments(StringBuilder sb, AppConfig config, FfmpegTranscodePlan transcodePlan)
        {
            if (!transcodePlan.HardwareEnabled)
            {
                sb.Append("-c:v ").Append(config.Media.VideoEncoder).Append(" -preset ").Append(config.Media.VideoPreset).Append(" -tune zerolatency -pix_fmt yuv420p ");
            }
            else if (string.Equals(transcodePlan.VideoEncoder, "h264_nvenc", StringComparison.OrdinalIgnoreCase))
            {
                // p4 是 NVENC 的均衡预设；ll 适合浏览器低延迟实时播放和回放定位后的首帧输出。
                sb.Append("-c:v h264_nvenc -preset p4 -tune ll -rc vbr ");
            }
            else if (string.Equals(transcodePlan.VideoEncoder, "h264_qsv", StringComparison.OrdinalIgnoreCase))
            {
                sb.Append("-c:v h264_qsv -preset veryfast ");
            }
            else if (string.Equals(transcodePlan.VideoEncoder, "h264_amf", StringComparison.OrdinalIgnoreCase))
            {
                sb.Append("-c:v h264_amf -usage lowlatency -quality speed ");
            }
            else
            {
                // 防御式回退，避免未来增加后端时构造出空编码器参数。
                sb.Append("-c:v ").Append(config.Media.VideoEncoder).Append(" -preset ").Append(config.Media.VideoPreset).Append(" -tune zerolatency -pix_fmt yuv420p ");
            }
            sb.Append("-profile:v high -level:v 4.1 -g 25 -keyint_min 25 -sc_threshold 0 ");
        }

        private static string DisplayBackendName(string value)
        {
            return string.Equals(value, "software", StringComparison.OrdinalIgnoreCase) ? "软件" : value;
        }

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
            StopProcess(false);
            _process.Dispose();
            _stop.Dispose();
        }

        private void StopProcess(bool immediate)
        {
            if (Interlocked.Exchange(ref _processStopped, 1) != 0) return;
            _stop.Cancel();
            try { _process.StandardInput.Close(); } catch { }
            if (!_process.HasExited)
            {
                if (immediate)
                {
                    try { _process.Kill(); } catch { }
                    try { _process.WaitForExit(300); } catch { }
                }
                else if (!_process.WaitForExit(1500)) { try { _process.Kill(); } catch { } }
            }
            try { if (_inputTask != null) _inputTask.Wait(immediate ? 300 : 500); } catch { }
            var exitDescription = "not-exited";
            try { if (_process.HasExited) exitDescription = _process.ExitCode.ToString(); } catch { }
            Log.Info("sid=" + _sid + " FFmpeg 转流进程已停止：退出码=" + exitDescription + "，耗时=" + _lifetime.ElapsedMilliseconds + "ms，输入编码=" + _inputVideoCodec + "，输入数据=" + Interlocked.Read(ref _inputBytes) + " 字节。");
        }

        private int SafeProcessId() { try { return _process.Id; } catch { return -1; } }
    }
}
