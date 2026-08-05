using System;
using System.Collections.Concurrent;

namespace HikSdkHttpBridge.Sessions
{
    // 已开始 HTTP 响应后无法再返回 JSON 错误，因此保留短期会话状态供 MSE 客户端在 EOF 后查询。
    internal sealed class SessionStatusRegistry
    {
        private const int RetentionMinutes = 10;
        private readonly ConcurrentDictionary<string, SessionStatus> _items = new ConcurrentDictionary<string, SessionStatus>(StringComparer.OrdinalIgnoreCase);

        public void SetActive(string sid)
        {
            Cleanup();
            var now = DateTime.UtcNow;
            _items[sid] = new SessionStatus { Sid = sid, Status = "active", EndReason = "active", StartedUtc = now, UpdatedUtc = now };
        }

        public void SetOutputReady(string sid)
        {
            SessionStatus value;
            if (!_items.TryGetValue(sid, out value)) return;
            value.OutputReadyUtc = DateTime.UtcNow;
            value.UpdatedUtc = value.OutputReadyUtc.Value;
        }

        // 浏览器通过 requestVideoFrameCallback（旧浏览器回退 loadeddata）回报首帧，
        // 用服务端单调的会话时间记录“HTTP 请求→实际渲染”的完整首画面耗时。
        public BrowserFrameTiming SetBrowserFirstFrame(string sid, long clientElapsedMs)
        {
            SessionStatus value;
            if (!_items.TryGetValue(sid, out value)) return null;
            var now = DateTime.UtcNow;
            if (!value.BrowserFirstFrameUtc.HasValue) value.BrowserFirstFrameUtc = now;
            value.BrowserFirstFrameClientElapsedMs = clientElapsedMs;
            value.UpdatedUtc = now;
            return new BrowserFrameTiming
            {
                TotalElapsedMs = (long)(value.BrowserFirstFrameUtc.Value - value.StartedUtc).TotalMilliseconds,
                OutputToFrameMs = value.OutputReadyUtc.HasValue ? (long)(value.BrowserFirstFrameUtc.Value - value.OutputReadyUtc.Value).TotalMilliseconds : -1
            };
        }

        public void SetEnded(string sid, string reason, string message, long queueCurrentBytes, long queuePeakBytes, long backpressureEvents)
        {
            Cleanup();
            SessionStatus value;
            if (!_items.TryGetValue(sid, out value)) value = new SessionStatus { Sid = sid, StartedUtc = DateTime.UtcNow };
            value.Status = "ended";
            value.EndReason = reason;
            value.Message = message ?? string.Empty;
            value.QueueCurrentBytes = queueCurrentBytes;
            value.QueuePeakBytes = queuePeakBytes;
            value.BackpressureEvents = backpressureEvents;
            value.UpdatedUtc = DateTime.UtcNow;
            _items[sid] = value;
        }

        public SessionStatus Get(string sid)
        {
            Cleanup();
            SessionStatus value;
            return _items.TryGetValue(sid, out value) ? value : null;
        }

        private void Cleanup()
        {
            var threshold = DateTime.UtcNow.AddMinutes(-RetentionMinutes);
            foreach (var item in _items)
            {
                if (item.Value.UpdatedUtc < threshold)
                {
                    SessionStatus ignored;
                    _items.TryRemove(item.Key, out ignored);
                }
            }
        }
    }

    internal sealed class SessionStatus
    {
        public string Sid { get; set; }
        public string Status { get; set; }
        public string EndReason { get; set; }
        public string Message { get; set; }
        public long QueueCurrentBytes { get; set; }
        public long QueuePeakBytes { get; set; }
        public long BackpressureEvents { get; set; }
        public DateTime UpdatedUtc { get; set; }
        public DateTime StartedUtc { get; set; }
        public DateTime? OutputReadyUtc { get; set; }
        public DateTime? BrowserFirstFrameUtc { get; set; }
        public long BrowserFirstFrameClientElapsedMs { get; set; }
    }

    internal sealed class BrowserFrameTiming
    {
        public long TotalElapsedMs { get; set; }
        public long OutputToFrameMs { get; set; }
    }
}
