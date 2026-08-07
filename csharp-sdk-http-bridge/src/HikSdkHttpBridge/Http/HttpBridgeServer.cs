using System;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using HikSdkHttpBridge.Config;
using HikSdkHttpBridge.Interop;
using HikSdkHttpBridge.Media;
using HikSdkHttpBridge.Model;
using HikSdkHttpBridge.Sessions;

namespace HikSdkHttpBridge.Http
{
    internal sealed class HttpBridgeServer : IDisposable
    {
        private readonly AppConfig _config;
        private readonly HttpListener _listener = new HttpListener();
        private readonly CancellationTokenSource _stop = new CancellationTokenSource();
        private readonly SessionRegistry _sessions;
        private readonly SessionStatusRegistry _sessionStatuses = new SessionStatusRegistry();
        private readonly HcNetSdkRuntime _sdk = new HcNetSdkRuntime();
        private readonly VideoCodecCache _codecCache;
        private FfmpegHardwareAcceleration _hardwareAcceleration = FfmpegHardwareAcceleration.Disabled;
        private Task _acceptTask;

        public HttpBridgeServer(AppConfig config)
        {
            _config = config;
            _sessions = new SessionRegistry(config.Server.MaxSessions);
            _codecCache = new VideoCodecCache(config.Media.CodecCacheSeconds, config.Media.CodecCacheFile);
            _listener.Prefixes.Add(string.Format("http://{0}:{1}/", config.Server.Bind, config.Server.Port));
        }

        public void Start()
        {
            _sdk.Initialize(_config.Sdk.HcnetDirectory);
            _hardwareAcceleration = FfmpegHardwareAcceleration.Detect(_config);
            _listener.Start();
            _acceptTask = Task.Run((Func<Task>)AcceptLoop);
            Log.Info(string.Format("HTTP 视频桥接服务已启动：地址=http://{0}:{1}/，SDK=HCNetSDK，进程=x86，硬件转码={2}", _config.Server.Bind, _config.Server.Port, _hardwareAcceleration.BackendName));
        }

        private async Task AcceptLoop()
        {
            while (!_stop.IsCancellationRequested)
            {
                HttpListenerContext context;
                try { context = await _listener.GetContextAsync().ConfigureAwait(false); }
                catch (Exception ex) when (_stop.IsCancellationRequested || ex is HttpListenerException || ex is ObjectDisposedException) { break; }
                _ = Task.Run(() => Handle(context));
            }
        }

        private async Task Handle(HttpListenerContext context)
        {
            if (!IsLocalIpv4Request(context.Request))
            {
                Log.Warn("已拒绝非本机 IPv4 客户端的 HTTP 请求。");
                context.Response.StatusCode = 403;
                context.Response.Close();
                return;
            }
            var requestId = context.Request.QueryString["sid"] ?? Guid.NewGuid().ToString("N");
            try
            {
                AddCommonHeaders(context.Response);
                if (context.Request.HttpMethod == "OPTIONS") { context.Response.StatusCode = 204; context.Response.Close(); return; }
                if (context.Request.HttpMethod != "GET") throw new BridgeException(405, "METHOD_NOT_ALLOWED", "only GET is supported");
                var path = context.Request.Url.AbsolutePath.TrimEnd('/').ToLowerInvariant();
                if (path == "/healthz" || path.Length == 0) { await WriteJson(context.Response, 200, new { status = "ok", sdk = "hcnetsdk", activeSessions = _sessions.Count, hardwareAcceleration = _hardwareAcceleration.BackendName }).ConfigureAwait(false); return; }
                if (path == "/version") { await WriteJson(context.Response, 200, new { name = "hik-sdk-http-bridge", version = "1.0.0", framework = ".NET Framework 4.8", architecture = "x86", hardwareAcceleration = _hardwareAcceleration.BackendName }).ConfigureAwait(false); return; }
                if (path == "/session-status")
                {
                    var sid = context.Request.QueryString["sid"];
                    if (string.IsNullOrWhiteSpace(sid)) throw new BridgeException(400, "INVALID_PARAMETER", "sid is required");
                    var status = _sessionStatuses.Get(sid);
                    if (status == null) { await WriteJson(context.Response, 404, new { code = "SESSION_NOT_FOUND", message = "session status is unavailable", requestId = sid }).ConfigureAwait(false); return; }
                    await WriteJson(context.Response, 200, new
                    {
                        sid = status.Sid,
                        status = status.Status,
                        endReason = status.EndReason,
                        message = status.Message,
                        queueCurrentBytes = status.QueueCurrentBytes,
                        queuePeakBytes = status.QueuePeakBytes,
                        backpressureEvents = status.BackpressureEvents,
                        outputReadyUtc = status.OutputReadyUtc.HasValue ? status.OutputReadyUtc.Value.ToString("o") : null,
                        browserFirstFrameUtc = status.BrowserFirstFrameUtc.HasValue ? status.BrowserFirstFrameUtc.Value.ToString("o") : null,
                        updatedUtc = status.UpdatedUtc.ToString("o")
                    }).ConfigureAwait(false);
                    return;
                }
                if (path == "/session-rendered")
                {
                    var sid = context.Request.QueryString["sid"];
                    if (string.IsNullOrWhiteSpace(sid)) throw new BridgeException(400, "INVALID_PARAMETER", "sid is required");
                    long clientElapsedMs;
                    if (!long.TryParse(context.Request.QueryString["elapsedMs"] ?? "0", out clientElapsedMs) || clientElapsedMs < 0 || clientElapsedMs > 600000)
                        throw new BridgeException(400, "INVALID_PARAMETER", "elapsedMs is invalid");
                    var timing = _sessionStatuses.SetBrowserFirstFrame(sid, clientElapsedMs);
                    if (timing == null) { await WriteJson(context.Response, 404, new { code = "SESSION_NOT_FOUND", message = "session status is unavailable", requestId = sid }).ConfigureAwait(false); return; }
                    Log.Info("sid=" + sid + " 浏览器首帧已渲染：服务端总耗时=" + timing.TotalElapsedMs + "ms，视频输出至渲染=" + timing.OutputToFrameMs + "ms，浏览器上报=" + clientElapsedMs + "ms。");
                    await WriteJson(context.Response, 200, new { status = "ok" }).ConfigureAwait(false);
                    return;
                }
                if (path != "/video") throw new BridgeException(404, "NOT_FOUND", "endpoint not found");
                var range = context.Request.Headers["Range"];
                var hasRange = !string.IsNullOrEmpty(range);
                if (hasRange) Log.Warn("requestId=" + requestId + " 浏览器发起了 Range 请求；视频流不支持服务端定位，将按新的不可定位流处理。");
                var streamRequest = StreamRequest.Parse(context.Request.QueryString, _config);
                await HandleVideo(context, streamRequest, hasRange).ConfigureAwait(false);
            }
            catch (BridgeException ex)
            {
                Log.Warn("requestId=" + requestId + " 请求失败：错误码=" + ex.Code + "，HTTP 状态=" + ex.HttpStatus);
                if (!context.Response.OutputStream.CanWrite) return;
                try { await WriteJson(context.Response, ex.HttpStatus, new { code = ex.Code, message = ex.Message, requestId }).ConfigureAwait(false); } catch { }
            }
            catch (Exception ex)
            {
                if (IsClientDisconnect(ex)) Log.Info("requestId=" + requestId + " 浏览器 HTTP 连接已断开。");
                else Log.Error("requestId=" + requestId + " 出现未处理异常：类型=" + ex.GetType().FullName + "，HRESULT=" + ex.HResult);
                try { if (context.Response.OutputStream.CanWrite) await WriteJson(context.Response, 500, new { code = "INTERNAL_ERROR", message = "internal server error", requestId }).ConfigureAwait(false); } catch { }
            }
            finally { try { context.Response.Close(); } catch { } }
        }

        private static bool IsLocalIpv4Request(HttpListenerRequest request)
        {
            var remoteEndPoint = request.RemoteEndPoint;
            return remoteEndPoint != null && IPAddress.Loopback.Equals(remoteEndPoint.Address);
        }

        private async Task HandleVideo(HttpListenerContext context, StreamRequest request, bool isRangeRetry)
        {
            var elapsed = Stopwatch.StartNew();
            long httpBytes = 0;
            using (_sessions.Reserve(request.Sid, isRangeRetry))
            {
                _sessionStatuses.SetActive(request.Sid);
                HcNetStreamSession session = null;
                FfmpegProcess ffmpeg = null;
                CodecProbe probe = null;
                var endReason = "active";
                var endMessage = string.Empty;
                try
                {
                    // 先登录并读取设备压缩参数。新设备可直接给出 H.264/H.265，从而只建立一条 NVR 取流会话。
                    session = new HcNetStreamSession(request, _config);
                    session.Prepare();
                    DetectedVideoCodec detectedCodec;
                    var cacheHit = _codecCache.TryGet(request, out detectedCodec);
                    if (cacheHit)
                    {
                        Log.Info("sid=" + request.Sid + " 命中视频编码缓存：编码=" + VideoCodecText.DisplayName(detectedCodec));
                    }
                    else if (session.ConfiguredVideoCodec != DetectedVideoCodec.Unknown)
                    {
                        detectedCodec = session.ConfiguredVideoCodec;
                        _codecCache.Set(request, detectedCodec);
                        Log.Info("sid=" + request.Sid + " 使用 NVR 编码配置直接建立视频管线，跳过实际码流探测。");
                    }
                    else
                    {
                        // 老设备或 IP 通道未提供压缩参数时才回退探测；探测与正式管线复用同一 SDK 登录和取流会话。
                        probe = await StartCodecProbe(request, session).ConfigureAwait(false);
                        detectedCodec = probe.Codec;
                        _codecCache.Set(request, detectedCodec);
                    }

                    // 仅 1 倍速 H.264 可零转码。快放/慢放必须通过 setpts 重写 fMP4 时间戳，故需要转码。
                    var outputMode = request.Speed == 1 && detectedCodec == DetectedVideoCodec.H264 ? VideoOutputMode.Copy : VideoOutputMode.H264Transcode;
                    var transcodePlan = _hardwareAcceleration.SelectPlan(outputMode, detectedCodec);
                    Log.Info("sid=" + request.Sid + " 编码处理决策：输入=" + VideoCodecText.DisplayName(detectedCodec) + "，输出=" + (outputMode == VideoOutputMode.Copy ? "H.264 直通" : "转码为 H.264") + "，转码后端=" + (transcodePlan.BackendName == "software" ? "软件" : transcodePlan.BackendName) + "，硬件解码=" + (transcodePlan.InputDecoder ?? "无"));
                    var reuseProbePipeline = probe != null && outputMode == VideoOutputMode.Copy;
                    byte[] prefix;
                    if (reuseProbePipeline)
                    {
                        prefix = probe.Prefix;
                        ffmpeg = probe.TakeFfmpeg();
                        probe.Dispose();
                        probe = null;
                        Log.Info("sid=" + request.Sid + " 复用 H.264 编码探测会话，避免重复登录和取流。");
                    }
                    else
                    {
                        if (probe != null)
                        {
                            probe.StopForPipelineSwitch();
                            probe.Dispose();
                            probe = null;
                            Log.Info("sid=" + request.Sid + " 编码探测 FFmpeg 已停止，复用同一 HCNetSDK 取流会话创建正式转码管线。");
                        }
                        Log.Info(string.Format("sid={0} 开始建立正式视频管线：模式={1}，设备={2}:{3}，请求通道={4}，码流={5}，速度={6}", request.Sid, request.Option == StreamOption.RealPlay ? "实时预览" : "回放", request.Ip, request.Port, request.Camera, request.Stream == StreamKind.Main ? "主码流" : "子码流", request.Speed));
                        if (!session.StreamStarted) session.Start();
                        ffmpeg = new FfmpegProcess(_config, request, outputMode, detectedCodec, _hardwareAcceleration);
                        ffmpeg.StartInput(session.Queue, _config.Timeouts.NoSdkDataMs);
                        prefix = await ReadInitialMp4(ffmpeg, _config.Timeouts.FirstMediaMs, request.Sid).ConfigureAwait(false);
                        ffmpeg.InspectInitialMp4(prefix);
                    }

                    // 个别旧 NVR 的压缩配置可能与实际 IP 通道码流不一致。以 fMP4 初始化段作最后校验；
                    // 若“配置 H.264”实际为 H.265，则不重新登录，只替换 FFmpeg 为 H.264 转码管线并修正缓存。
                    if (outputMode == VideoOutputMode.Copy && ffmpeg.DetectedVideoCodec == DetectedVideoCodec.H265)
                    {
                        Log.Warn("sid=" + request.Sid + " NVR 编码配置与实际码流不一致：配置为 H.264，实际为 H.265；将复用当前 HCNetSDK 会话切换为 H.264 转码。");
                        _codecCache.Set(request, DetectedVideoCodec.H265);
                        ffmpeg.StopForPipelineSwitch();
                        ffmpeg.Dispose();
                        outputMode = VideoOutputMode.H264Transcode;
                        ffmpeg = new FfmpegProcess(_config, request, outputMode, DetectedVideoCodec.H265, _hardwareAcceleration);
                        ffmpeg.StartInput(session.Queue, _config.Timeouts.NoSdkDataMs);
                        prefix = await ReadInitialMp4(ffmpeg, _config.Timeouts.FirstMediaMs, request.Sid).ConfigureAwait(false);
                        ffmpeg.InspectInitialMp4(prefix);
                    }

                    Log.Info("sid=" + request.Sid + " 视频输出已就绪，开始向浏览器传输：初始化数据=" + prefix.Length + " 字节，耗时=" + elapsed.ElapsedMilliseconds + "ms");
                    _sessionStatuses.SetOutputReady(request.Sid);
                    context.Response.StatusCode = 200;
                    context.Response.ContentType = "video/mp4";
                    context.Response.SendChunked = true;
                    context.Response.KeepAlive = true;
                    context.Response.Headers["Cache-Control"] = "no-store, no-cache";
                    context.Response.Headers["Accept-Ranges"] = "none";
                    context.Response.Headers["X-Hik-Bridge-Mse-Codecs"] = ffmpeg.MseCodecs;
                    context.Response.Headers["X-Hik-Bridge-Video-Codec"] = "h264";
                    context.Response.Headers["X-Hik-Bridge-Playback-Start"] = request.Option == StreamOption.Playback ? request.Start.ToString() : "";
                    context.Response.Headers["X-Hik-Bridge-Playback-End"] = request.Option == StreamOption.Playback ? request.End.ToString() : "";
                    context.Response.Headers["X-Hik-Bridge-Session-Id"] = request.Sid;
                    context.Response.Headers["X-Hik-Bridge-Session-Status-Url"] = "/session-status?sid=" + Uri.EscapeDataString(request.Sid);
                    await context.Response.OutputStream.WriteAsync(prefix, 0, prefix.Length).ConfigureAwait(false);
                    httpBytes += prefix.Length;
                    await context.Response.OutputStream.FlushAsync().ConfigureAwait(false);
                    var buffer = new byte[64 * 1024];
                    while (!_stop.IsCancellationRequested)
                    {
                        var count = await ffmpeg.Output.ReadAsync(buffer, 0, buffer.Length).ConfigureAwait(false);
                        if (count <= 0)
                        {
                            endReason = ResolveEndReason(session, ffmpeg);
                            endMessage = EndReasonMessage(endReason);
                            break;
                        }
                        await context.Response.OutputStream.WriteAsync(buffer, 0, count).ConfigureAwait(false);
                        httpBytes += count;
                        await context.Response.OutputStream.FlushAsync().ConfigureAwait(false);
                    }
                }
                catch (Exception ex) when (IsClientDisconnect(ex))
                {
                    endReason = "client_replaced";
                    endMessage = "浏览器主动停止或替换了视频流";
                    Log.Info("sid=" + request.Sid + " 浏览器已主动停止或替换视频流，开始释放资源。");
                }
                catch (BridgeException)
                {
                    endReason = "startup_failed";
                    endMessage = "Bridge 建流失败";
                    throw;
                }
                finally
                {
                    if (endReason == "active")
                    {
                        endReason = _stop.IsCancellationRequested ? "server_stopped" : ResolveEndReason(session, ffmpeg);
                        endMessage = EndReasonMessage(endReason);
                    }
                    _sessionStatuses.SetEnded(request.Sid, endReason, endMessage, session == null ? 0 : session.QueueCurrentBytes, session == null ? 0 : session.QueuePeakBytes, session == null ? 0 : session.QueueBackpressureEvents);
                    Log.Info("sid=" + request.Sid + " 视频管线结束：原因=" + endReason + "，HTTP 输出=" + httpBytes + " 字节，耗时=" + elapsed.ElapsedMilliseconds + "ms，队列当前/峰值=" + (session == null ? 0 : session.QueueCurrentBytes) + "/" + (session == null ? 0 : session.QueuePeakBytes) + " 字节，背压次数=" + (session == null ? 0 : session.QueueBackpressureEvents));
                    if (probe != null) probe.Dispose();
                    if (ffmpeg != null) ffmpeg.Dispose();
                    if (session != null) session.Dispose();
                }
            }
        }

        private static string ResolveEndReason(HcNetStreamSession session, FfmpegProcess ffmpeg)
        {
            if (session != null && !string.Equals(session.EndReason, "active", StringComparison.OrdinalIgnoreCase)) return session.EndReason;
            if (ffmpeg != null && !string.Equals(ffmpeg.InputEndReason, "active", StringComparison.OrdinalIgnoreCase)) return ffmpeg.InputEndReason;
            if (ffmpeg != null && ffmpeg.HasExited) return "ffmpeg_exit";
            return "stream_ended";
        }

        private static string EndReasonMessage(string reason)
        {
            switch (reason)
            {
                case "playback_end": return "回放已到达请求的结束时间";
                case "queue_overflow": return "HCNetSDK 媒体队列在限定背压后仍持续满载";
                case "sdk_data_timeout": return "HCNetSDK 未继续输出媒体数据";
                case "sdk_keepalive_failed": return "HCNetSDK 回放保活连续失败";
                case "ffmpeg_exit": return "FFmpeg 在视频结束前退出";
                case "playback_position_error": return "HCNetSDK 回放位置状态异常";
                case "client_replaced": return "浏览器主动停止或替换了视频流";
                case "server_stopped": return "桥接服务正在停止";
                default: return "视频流异常结束";
            }
        }

        private async Task<CodecProbe> StartCodecProbe(StreamRequest request, HcNetStreamSession session)
        {
            FfmpegProcess ffmpeg = null;
            try
            {
                ffmpeg = new FfmpegProcess(_config, request, VideoOutputMode.Copy, DetectedVideoCodec.Unknown, FfmpegHardwareAcceleration.Disabled);
                Log.Info("sid=" + request.Sid + " 开始探测输入视频编码。");
                session.Start();
                ffmpeg.StartInput(session.Queue, _config.Timeouts.NoSdkDataMs);
                var prefix = await ReadInitialMp4(ffmpeg, _config.Timeouts.FirstMediaMs, request.Sid + "-probe").ConfigureAwait(false);
                ffmpeg.InspectInitialMp4(prefix);
                Log.Info("sid=" + request.Sid + " 输入视频编码探测完成：编码=" + VideoCodecText.DisplayName(ffmpeg.DetectedVideoCodec) + "，初始化数据=" + prefix.Length + " 字节。");
                return new CodecProbe(ffmpeg, prefix, ffmpeg.DetectedVideoCodec);
            }
            catch
            {
                if (ffmpeg != null) ffmpeg.Dispose();
                throw;
            }
        }

        private sealed class CodecProbe : IDisposable
        {
            private FfmpegProcess _ffmpeg;

            public CodecProbe(FfmpegProcess ffmpeg, byte[] prefix, DetectedVideoCodec codec)
            {
                _ffmpeg = ffmpeg;
                Prefix = prefix;
                Codec = codec;
            }

            public byte[] Prefix { get; private set; }
            public DetectedVideoCodec Codec { get; private set; }
            public FfmpegProcess TakeFfmpeg() { var value = _ffmpeg; _ffmpeg = null; return value; }
            public void StopForPipelineSwitch() { if (_ffmpeg != null) _ffmpeg.StopForPipelineSwitch(); }

            public void Dispose()
            {
                if (_ffmpeg != null) { _ffmpeg.Dispose(); _ffmpeg = null; }
            }
        }

        private static async Task<byte[]> ReadInitialMp4(FfmpegProcess ffmpeg, int timeoutMs, string sid)
        {
            using (var memory = new MemoryStream())
            using (var cancellation = new CancellationTokenSource(timeoutMs))
            {
                var buffer = new byte[32 * 1024];
                try
                {
                    while (memory.Length < 4 * 1024 * 1024)
                    {
                        var count = await ffmpeg.Output.ReadAsync(buffer, 0, buffer.Length, cancellation.Token).ConfigureAwait(false);
                        if (count <= 0) break;
                        memory.Write(buffer, 0, count);
                        var bytes = memory.GetBuffer();
                        var length = (int)memory.Length;
                        if (ContainsAscii(bytes, length, "ftyp") && ContainsAscii(bytes, length, "moov")) return memory.ToArray();
                    }
                }
                catch (OperationCanceledException) { throw new BridgeException(504, "FFMPEG_OUTPUT_TIMEOUT", "waiting for fragmented MP4 timed out"); }
                var details = ffmpeg.ErrorText;
                if (details.Length > 600) details = details.Substring(details.Length - 600);
                throw new BridgeException(502, "FFMPEG_NO_MP4", "FFmpeg did not produce a valid MP4 header" + (details.Length == 0 ? "" : ": " + details.Trim()));
            }
        }

        private static bool ContainsAscii(byte[] data, int length, string text)
        {
            var target = Encoding.ASCII.GetBytes(text);
            for (var i = 0; i <= length - target.Length; i++)
            {
                var matched = true;
                for (var j = 0; j < target.Length; j++) if (data[i + j] != target[j]) { matched = false; break; }
                if (matched) return true;
            }
            return false;
        }

        private static void AddCommonHeaders(HttpListenerResponse response)
        {
            response.Headers["Access-Control-Allow-Origin"] = "*";
            response.Headers["Access-Control-Allow-Methods"] = "GET, OPTIONS";
            response.Headers["Access-Control-Allow-Headers"] = "Content-Type";
            response.Headers["Access-Control-Expose-Headers"] = "X-Hik-Bridge-Mse-Codecs, X-Hik-Bridge-Video-Codec, X-Hik-Bridge-Playback-Start, X-Hik-Bridge-Playback-End, X-Hik-Bridge-Session-Id, X-Hik-Bridge-Session-Status-Url";
            response.Headers["X-Content-Type-Options"] = "nosniff";
        }

        private static bool IsClientDisconnect(Exception exception)
        {
            var http = exception as HttpListenerException;
            if (http != null) return http.ErrorCode == 64 || http.ErrorCode == 995 || http.ErrorCode == 1229 || http.NativeErrorCode == 64;
            var io = exception as IOException;
            if (io != null && io.InnerException != null) return IsClientDisconnect(io.InnerException);
            return exception is ObjectDisposedException;
        }

        private static async Task WriteJson(HttpListenerResponse response, int status, object value)
        {
            if (response.HeadersSent()) return;
            var json = new JavaScriptSerializer().Serialize(value);
            var bytes = Encoding.UTF8.GetBytes(json);
            response.StatusCode = status;
            response.ContentType = "application/json; charset=utf-8";
            response.ContentLength64 = bytes.Length;
            await response.OutputStream.WriteAsync(bytes, 0, bytes.Length).ConfigureAwait(false);
            response.Close();
        }

        public void Dispose()
        {
            _stop.Cancel();
            try { _listener.Stop(); } catch { }
            try { if (_acceptTask != null) _acceptTask.Wait(1000); } catch { }
            _listener.Close();
            _sdk.Dispose();
            _stop.Dispose();
        }
    }

    internal static class HttpListenerResponseExtensions
    {
        public static bool HeadersSent(this HttpListenerResponse response)
        {
            try { return !response.OutputStream.CanWrite; } catch { return true; }
        }
    }
}
