using System;
using System.Diagnostics;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using HikSdkHttpBridge.Config;
using HikSdkHttpBridge.Interop;
using HikSdkHttpBridge.Media;
using HikSdkHttpBridge.Model;

namespace HikSdkHttpBridge.Sessions
{
    internal sealed class HcNetStreamSession : IDisposable
    {
        private readonly StreamRequest _request;
        private readonly AppConfig _config;
        private readonly ByteBoundedQueue _queue;
        private readonly RealDataCallback _realCallback;
        private readonly PlayDataCallback _playCallback;
        private readonly ManualResetEventSlim _firstData = new ManualResetEventSlim(false);
        private readonly CancellationTokenSource _stop = new CancellationTokenSource();
        private readonly Stopwatch _lifetime = Stopwatch.StartNew();
        private int _userId = -1;
        private int _streamHandle = -1;
        private int _disposed;
        private int _overflow;
        private long _sdkBytes;
        private long _mediaPackets;
        private long _headerPackets;
        private int _firstMediaSize;
        private uint _firstMediaType;
        private string _endReason = "active";
        private ResolvedChannelType _resolvedChannelType = ResolvedChannelType.Unknown;
        private int _sdkCamera;
        private int _keepAliveFailures;
        private bool _prepared;
        private bool _streamStarted;
        private DetectedVideoCodec _configuredVideoCodec = DetectedVideoCodec.Unknown;

        public HcNetStreamSession(StreamRequest request, AppConfig config)
        {
            _request = request;
            _config = config;
            _queue = new ByteBoundedQueue(config.Media.QueueBytes);
            _realCallback = OnRealData;
            _playCallback = OnPlayData;
        }

        public ByteBoundedQueue Queue { get { return _queue; } }
        public string EndReason { get { return _endReason; } }
        public long QueueCurrentBytes { get { return _queue.CurrentBytes; } }
        public long QueuePeakBytes { get { return _queue.PeakBytes; } }
        public long QueueBackpressureEvents { get { return _queue.BackpressureEvents; } }
        // 若 NVR 支持 NET_DVR_GET_COMPRESSCFG_V30，则在真正取流前即可得知主/子码流的编码类型。
        public DetectedVideoCodec ConfiguredVideoCodec { get { return _configuredVideoCodec; } }
        public bool StreamStarted { get { return _streamStarted; } }

        public void Start()
        {
            Prepare();
            StartStreamAndWaitForData();
        }

        // 将登录、通道解析和编码参数读取与实际取流拆开，供桥接端在创建 FFmpeg 前选择正确管线。
        public void Prepare()
        {
            if (_prepared) return;
            Login();
            ReadVideoStreamConfiguration();
            _prepared = true;
        }

        public void StartStreamAndWaitForData()
        {
            Prepare();
            if (!_streamStarted)
            {
                if (_request.Option == StreamOption.RealPlay) StartRealPlay(); else StartPlayback();
                _streamStarted = true;
                Log.Info("sid=" + _request.Sid + " NVR 通道建流已提交：模式=" + (_request.Option == StreamOption.RealPlay ? "实时预览" : "视频回放") + "，请求通道=" + _request.Camera + "，请求类型=" + ChannelTypeText(_request.ChannelType) + "，实际通道=" + _sdkCamera + "，实际类型=" + ResolvedChannelTypeText(_resolvedChannelType) + "，码流=" + (_request.Stream == StreamKind.Main ? "主码流" : "子码流") + "，速度=" + _request.Speed + "x。");
            }
            var wait = Stopwatch.StartNew();
            if (!_firstData.Wait(_config.Timeouts.SdkStartMs))
            {
                Log.Warn("sid=" + _request.Sid + " HCNetSDK 取流超时：等待=" + wait.ElapsedMilliseconds + "ms，头包=" + Interlocked.Read(ref _headerPackets) + "，媒体包=" + Interlocked.Read(ref _mediaPackets) + "，数据=" + Interlocked.Read(ref _sdkBytes) + " 字节。");
                if (Volatile.Read(ref _overflow) != 0) throw new BridgeException(502, "MEDIA_QUEUE_OVERFLOW", "HCNetSDK media queue backpressure limit exceeded");
                throw new BridgeException(504, "SDK_STREAM_TIMEOUT", "waiting for HCNetSDK stream timed out");
            }
            Log.Info("sid=" + _request.Sid + " HCNetSDK 已获取首个视频数据：等待=" + wait.ElapsedMilliseconds + "ms，首包=" + _firstMediaSize + " 字节，媒体包=" + Interlocked.Read(ref _mediaPackets) + "。");
        }

        private void Login()
        {
            ProbeNvrEndpoint();
            var elapsed = Stopwatch.StartNew();
            Log.Info("sid=" + _request.Sid + " 开始登录 NVR：设备=" + _request.Ip + ":" + _request.Port + "。");
            var login = NetDvrUserLoginInfo.Create();
            CopyAnsi(_request.Ip, login.DeviceAddress);
            CopyAnsi(_request.UserName, login.UserName);
            CopyAnsi(_request.Password, login.Password);
            login.Port = _request.Port;
            login.UseAsyncLogin = 0;
            login.LoginMode = 0;
            var deviceInfo = NetDvrDeviceInfoV40.Create();
            _userId = NativeMethods.NET_DVR_Login_V40(ref login, out deviceInfo);
            if (_userId < 0)
            {
                var sdkError = NativeMethods.NET_DVR_GetLastError();
                // HCNetSDK 错误 7 表示设备连接失败；与预检保持同一错误码，便于前端给出明确提示。
                if (sdkError == 7) throw NvrUnreachableError();
                throw new BridgeException(502, "SDK_LOGIN_FAILED", "NVR 登录失败：HCNetSDK 错误=" + sdkError + "。请检查用户名、密码、设备状态和登录权限。");
            }
            Log.Info("sid=" + _request.Sid + " NVR 登录成功：耗时=" + elapsed.ElapsedMilliseconds + "ms。");
            var topology = NvrChannelTopology.Read(_userId, deviceInfo, _request.Sid);
            _sdkCamera = topology.Resolve(_request.Camera, _request.ChannelType, out _resolvedChannelType);
            Log.Info("sid=" + _request.Sid + " SDK 通道已解析：请求通道=" + _request.Camera + "，请求类型=" + ChannelTypeText(_request.ChannelType) + "，实际通道=" + _sdkCamera + "，实际类型=" + ResolvedChannelTypeText(_resolvedChannelType) + "。");
        }

        // 在调用 HCNetSDK 前先验证其私有 SDK 端口可达，避免把网络故障误报为登录或通道故障。
        private void ProbeNvrEndpoint()
        {
            var elapsed = Stopwatch.StartNew();
            Log.Info("sid=" + _request.Sid + " 开始检测 NVR SDK 端口可达性：设备=" + _request.Ip + ":" + _request.Port + "，超时=" + _config.Sdk.ConnectProbeTimeoutMs + "ms。");
            try
            {
                using (var client = new TcpClient())
                {
                    var result = client.BeginConnect(_request.Ip, _request.Port, null, null);
                    if (!result.AsyncWaitHandle.WaitOne(_config.Sdk.ConnectProbeTimeoutMs))
                    {
                        Log.Warn("sid=" + _request.Sid + " NVR SDK 端口连接超时：设备=" + _request.Ip + ":" + _request.Port + "，等待=" + elapsed.ElapsedMilliseconds + "ms。");
                        throw NvrUnreachableError();
                    }
                    client.EndConnect(result);
                }
                Log.Info("sid=" + _request.Sid + " NVR SDK 端口可达：设备=" + _request.Ip + ":" + _request.Port + "，耗时=" + elapsed.ElapsedMilliseconds + "ms。");
            }
            catch (BridgeException) { throw; }
            catch (SocketException ex)
            {
                Log.Warn("sid=" + _request.Sid + " NVR SDK 端口不可达：设备=" + _request.Ip + ":" + _request.Port + "，原因=" + SocketErrorText(ex.SocketErrorCode) + "，耗时=" + elapsed.ElapsedMilliseconds + "ms。");
                throw NvrUnreachableError();
            }
            catch (Exception)
            {
                Log.Warn("sid=" + _request.Sid + " NVR SDK 端口检测失败：设备=" + _request.Ip + ":" + _request.Port + "，发生未分类的连接异常，耗时=" + elapsed.ElapsedMilliseconds + "ms。");
                throw NvrUnreachableError();
            }
        }

        private BridgeException NvrUnreachableError()
        {
            return new BridgeException(502, "NVR_UNREACHABLE", "无法连接 NVR " + _request.Ip + ":" + _request.Port + " 的 SDK 端口；请检查设备 IP、SDK 端口、网络连通性或防火墙。");
        }

        private static string SocketErrorText(SocketError error)
        {
            switch (error)
            {
                case SocketError.ConnectionRefused: return "目标端口拒绝连接";
                case SocketError.TimedOut: return "连接超时";
                case SocketError.HostUnreachable: return "目标主机不可达";
                case SocketError.NetworkUnreachable: return "网络不可达";
                case SocketError.HostNotFound: return "无法解析目标主机";
                case SocketError.TryAgain: return "目标地址解析失败";
                case SocketError.AddressNotAvailable: return "目标地址不可用";
                default: return "套接字错误代码=" + (int)error;
            }
        }

        private void ReadVideoStreamConfiguration()
        {
            // 读取失败只代表旧设备或 IP 通道不支持该配置接口，后续会自动回退到实际码流探测。
            if (!_config.Sdk.ReadVideoCodecFromDevice && (_request.Option != StreamOption.RealPlay || _config.Sdk.RealPlayKeyFrameIntervalFrames <= 0)) return;
            var compression = NetDvrCompressionCfgV30.Create();
            uint bytesReturned;
            if (!NativeMethods.NET_DVR_GetDVRConfig(_userId, 1040, _sdkCamera, ref compression, compression.Size, out bytesReturned))
            {
                Log.Info("sid=" + _request.Sid + " 未读取到 NVR 码流编码参数，将按实际视频数据探测：HCNetSDK 错误=" + NativeMethods.NET_DVR_GetLastError() + "。");
                return;
            }

            var selected = _request.Stream == StreamKind.Main ? compression.NormalHighRecord : compression.Network;
            if (_config.Sdk.ReadVideoCodecFromDevice)
            {
                _configuredVideoCodec = ToDetectedCodec(selected.VideoEncodingType);
                if (_configuredVideoCodec != DetectedVideoCodec.Unknown)
                    Log.Info("sid=" + _request.Sid + " 已从 NVR 读取码流编码：码流=" + (_request.Stream == StreamKind.Main ? "主码流" : "子码流") + "，编码=" + VideoCodecText.DisplayName(_configuredVideoCodec) + "。");
                else
                    Log.Info("sid=" + _request.Sid + " NVR 未返回可识别的视频编码类型，将按实际视频数据探测。");
            }

            if (_request.Option != StreamOption.RealPlay || _config.Sdk.RealPlayKeyFrameIntervalFrames <= 0) return;
            var requestedInterval = (ushort)_config.Sdk.RealPlayKeyFrameIntervalFrames;
            // 只缩短自动/较长的间隔，绝不覆盖已经更短的设备设置。
            if (selected.IntervalFrameI != 0 && selected.IntervalFrameI != 0xfffe && selected.IntervalFrameI <= requestedInterval) return;
            selected.IntervalFrameI = requestedInterval;
            if (_request.Stream == StreamKind.Main) compression.NormalHighRecord = selected; else compression.Network = selected;
            if (NativeMethods.NET_DVR_SetDVRConfig(_userId, 1041, _sdkCamera, ref compression, compression.Size))
                Log.Info("sid=" + _request.Sid + " 已将 NVR " + (_request.Stream == StreamKind.Main ? "主码流" : "子码流") + " I 帧间隔缩短为 " + requestedInterval + " 帧。");
            else
                Log.Warn("sid=" + _request.Sid + " 无法修改 NVR I 帧间隔，将继续使用设备当前设置：HCNetSDK 错误=" + NativeMethods.NET_DVR_GetLastError() + "。");
        }

        private static DetectedVideoCodec ToDetectedCodec(byte value)
        {
            // HCNetSDK: 0=私有 H.264，1=标准 H.264，10=标准 H.265。
            if (value == 0 || value == 1) return DetectedVideoCodec.H264;
            if (value == 10) return DetectedVideoCodec.H265;
            return DetectedVideoCodec.Unknown;
        }

        private void StartRealPlay()
        {
            var preview = NetDvrPreviewInfo.Create();
            preview.Channel = _sdkCamera;
            preview.StreamType = _request.Stream == StreamKind.Main ? 0u : 1u;
            preview.LinkMode = 0;
            preview.PlayWindow = IntPtr.Zero;
            preview.Blocked = 1;
            preview.DisplayBufferCount = 1;
            _streamHandle = NativeMethods.NET_DVR_RealPlay_V40(_userId, ref preview, _realCallback, IntPtr.Zero);
            if (_streamHandle < 0) throw SdkError(502, "SDK_REALPLAY_FAILED", "NET_DVR_RealPlay_V40");
            Log.Info("sid=" + _request.Sid + " 已启动实时取流：通道=" + _sdkCamera + "，码流=" + (_request.Stream == StreamKind.Main ? "主码流" : "子码流") + "，传输=TCP。");
            RequestRealplayKeyFrame();
        }

        private void RequestRealplayKeyFrame()
        {
            if (!_config.Sdk.ForceKeyFrameOnRealPlay) return;
            var success = _request.Stream == StreamKind.Main
                ? NativeMethods.NET_DVR_MakeKeyFrame(_userId, _sdkCamera)
                : NativeMethods.NET_DVR_MakeKeyFrameSub(_userId, _sdkCamera);
            if (success)
                Log.Info("sid=" + _request.Sid + " 已请求 NVR 立即发送 " + (_request.Stream == StreamKind.Main ? "主码流" : "子码流") + "关键帧。");
            else
                Log.Info("sid=" + _request.Sid + " NVR 不支持或拒绝立即发送关键帧，将等待设备自然关键帧：HCNetSDK 错误=" + NativeMethods.NET_DVR_GetLastError() + "。");
        }

        private void StartPlayback()
        {
            var vod = new NetDvrVodPara
            {
                Size = (uint)Marshal.SizeOf(typeof(NetDvrVodPara)),
                StreamInfo = NetDvrStreamInfo.Create(_sdkCamera),
                BeginTime = NetDvrTime.FromUnixSeconds(_request.Start),
                EndTime = NetDvrTime.FromUnixSeconds(_request.End),
                Window = IntPtr.Zero,
                StreamType = (byte)(_request.Stream == StreamKind.Main ? 0 : 1),
                Reserved = new byte[19]
            };
            _streamHandle = NativeMethods.NET_DVR_PlayBackByTime_V40(_userId, ref vod);
            if (_streamHandle < 0) throw SdkError(502, "SDK_PLAYBACK_FAILED", "NET_DVR_PlayBackByTime_V40");
            if (!NativeMethods.NET_DVR_SetPlayDataCallBack_V40(_streamHandle, _playCallback, IntPtr.Zero)) throw SdkError(502, "SDK_CALLBACK_FAILED", "NET_DVR_SetPlayDataCallBack_V40");
            Control(NativeMethods.PlayStart, "NET_DVR_PLAYSTART");
            ApplySpeed();
            Log.Info("sid=" + _request.Sid + " 已启动回放取流：通道=" + _sdkCamera + "，开始=" + _request.Start + "，结束=" + _request.End + "，速度=" + _request.Speed + "x。");
            _ = Task.Run((Action)MonitorPlayback);
        }

        private void ApplySpeed()
        {
            if (_request.Speed == 1) { return; }
            var command = _request.Speed > 1 ? NativeMethods.PlayFast : NativeMethods.PlaySlow;
            var steps = (int)Math.Round(Math.Abs(Math.Log(_request.Speed, 2)));
            for (var i = 0; i < steps; i++) Control(command, command == NativeMethods.PlayFast ? "NET_DVR_PLAYFAST" : "NET_DVR_PLAYSLOW");
        }

        private void Control(uint command, string name)
        {
            if (!NativeMethods.NET_DVR_PlayBackControl_V40(_streamHandle, command, IntPtr.Zero, 0, IntPtr.Zero, IntPtr.Zero)) throw SdkError(502, "SDK_PLAYBACK_CONTROL_FAILED", name);
        }

        private void MonitorPlayback()
        {
            var monitor = Stopwatch.StartNew();
            long nextPositionCheckMs = 0;
            long nextKeepAliveMs = _config.Timeouts.PlaybackKeepAliveMs;
            while (!_stop.IsCancellationRequested)
            {
                Thread.Sleep(200);
                if (_streamHandle < 0) return;
                var elapsedMs = monitor.ElapsedMilliseconds;
                if (elapsedMs >= nextKeepAliveMs)
                {
                    SendPlaybackKeepAlive();
                    nextKeepAliveMs = elapsedMs + _config.Timeouts.PlaybackKeepAliveMs;
                    if (!string.Equals(_endReason, "active", StringComparison.OrdinalIgnoreCase)) return;
                }
                if (elapsedMs < nextPositionCheckMs) continue;
                nextPositionCheckMs = elapsedMs + 500;
                var position = NativeMethods.NET_DVR_GetPlayBackPos(_streamHandle);
                if (position == 100) { CompletePlayback("playback_end", "回放已到达结束位置。", false); return; }
                if (position == 200) { CompletePlayback("playback_position_error", "回放位置状态异常。", true); return; }
            }
        }

        private void SendPlaybackKeepAlive()
        {
            if (NativeMethods.NET_DVR_PlayBackControl_V40(_streamHandle, NativeMethods.PlayKeepAlive, IntPtr.Zero, 0, IntPtr.Zero, IntPtr.Zero))
            {
                if (_keepAliveFailures > 0) Log.Info("sid=" + _request.Sid + " 回放保活已恢复：此前连续失败=" + _keepAliveFailures + " 次。");
                _keepAliveFailures = 0;
                return;
            }
            var failures = Interlocked.Increment(ref _keepAliveFailures);
            var error = NativeMethods.NET_DVR_GetLastError();
            Log.Warn("sid=" + _request.Sid + " 回放保活失败：连续失败=" + failures + " 次，HCNetSDK 错误=" + error + "。");
            // 一次失败可能只是瞬时网络波动；连续失败则让前端以实际播放位置重建会话。
            if (failures >= 3) CompletePlayback("sdk_keepalive_failed", "回放保活连续失败，结束当前会话以便前端恢复。", true);
        }

        private void CompletePlayback(string reason, string message, bool warning)
        {
            if (!string.Equals(_endReason, "active", StringComparison.OrdinalIgnoreCase)) return;
            _endReason = reason;
            if (warning) Log.Warn("sid=" + _request.Sid + " " + message);
            else Log.Info("sid=" + _request.Sid + " " + message);
            _queue.Complete();
        }

        private void OnRealData(int handle, uint dataType, IntPtr buffer, uint size, IntPtr user) { AcceptData(dataType, buffer, size); }
        private void OnPlayData(int handle, uint dataType, IntPtr buffer, uint size, IntPtr user) { AcceptData(dataType, buffer, size); }

        private void AcceptData(uint dataType, IntPtr buffer, uint size)
        {
            if (Volatile.Read(ref _disposed) != 0 || buffer == IntPtr.Zero || size == 0) return;
            if (!string.Equals(_endReason, "active", StringComparison.OrdinalIgnoreCase)) return;
            if (dataType != NativeMethods.SystemHeader && dataType != NativeMethods.StreamData && dataType != NativeMethods.StandardVideoData) return;
            if (size > int.MaxValue) return;
            var data = new byte[(int)size];
            Marshal.Copy(buffer, data, 0, data.Length);
            Interlocked.Add(ref _sdkBytes, data.Length);
            if (dataType == NativeMethods.SystemHeader) Interlocked.Increment(ref _headerPackets);
            else
            {
                Interlocked.Increment(ref _mediaPackets);
                if (Interlocked.CompareExchange(ref _firstMediaSize, data.Length, 0) == 0) _firstMediaType = dataType;
            }
            if (!_queue.TryEnqueue(data, _config.Media.QueueBackpressureMs))
            {
                Interlocked.Exchange(ref _overflow, 1);
                _endReason = "queue_overflow";
                Log.Warn("sid=" + _request.Sid + " HCNetSDK 媒体队列背压超限：当前/峰值=" + _queue.CurrentBytes + "/" + _queue.PeakBytes + " 字节，容量=" + _config.Media.QueueBytes + " 字节，背压次数=" + _queue.BackpressureEvents + "，等待=" + _config.Media.QueueBackpressureMs + "ms；结束会话以便前端恢复。");
                _queue.Complete();
                return;
            }
            if (dataType != NativeMethods.SystemHeader) _firstData.Set();
        }

        private static void CopyAnsi(string value, byte[] target)
        {
            var source = Encoding.Default.GetBytes(value);
            if (source.Length >= target.Length) throw new BridgeException(400, "INVALID_PARAMETER", "NVR connection field exceeds HCNetSDK byte limit");
            Buffer.BlockCopy(source, 0, target, 0, source.Length);
        }

        private static string ChannelTypeText(ChannelType value)
        {
            switch (value)
            {
                case ChannelType.Analog: return "模拟通道";
                case ChannelType.Digital: return "数字通道";
                case ChannelType.DigitalIndex: return "数字通道序号";
                default: return "自动";
            }
        }

        private static string ResolvedChannelTypeText(ResolvedChannelType value)
        {
            switch (value)
            {
                case ResolvedChannelType.Analog: return "模拟通道";
                case ResolvedChannelType.Digital: return "数字通道";
                default: return "未识别";
            }
        }

        private static BridgeException SdkError(int status, string code, string operation)
        {
            return new BridgeException(status, code, operation + " failed, HCNetSDK error=" + NativeMethods.NET_DVR_GetLastError());
        }

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
            _stop.Cancel();
            if (_streamHandle >= 0)
            {
                if (_request.Option == StreamOption.RealPlay) NativeMethods.NET_DVR_StopRealPlay(_streamHandle);
                else NativeMethods.NET_DVR_StopPlayBack(_streamHandle);
                _streamHandle = -1;
            }
            if (_userId >= 0) { NativeMethods.NET_DVR_Logout(_userId); _userId = -1; }
            _queue.Complete();
            Log.Info("sid=" + _request.Sid + " HCNetSDK 会话已释放：原因=" + _endReason + "，耗时=" + _lifetime.ElapsedMilliseconds + "ms，媒体包=" + Interlocked.Read(ref _mediaPackets) + "，数据=" + Interlocked.Read(ref _sdkBytes) + " 字节，队列峰值=" + _queue.PeakBytes + " 字节，背压次数=" + _queue.BackpressureEvents + "。");
            _firstData.Dispose();
            _stop.Dispose();
        }
    }
}
