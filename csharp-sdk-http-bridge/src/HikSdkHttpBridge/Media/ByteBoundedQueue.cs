using System;
using System.Collections.Generic;
using System.Threading;

namespace HikSdkHttpBridge.Media
{
    internal sealed class ByteBoundedQueue : IDisposable
    {
        private readonly object _sync = new object();
        private readonly Queue<byte[]> _items = new Queue<byte[]>();
        private readonly long _capacity;
        private long _bytes;
        private bool _completed;
        private long _peakBytes;
        private long _backpressureEvents;

        public ByteBoundedQueue(long capacity) { _capacity = capacity; }

        // HCNetSDK 在数据回调线程中执行。这里仅进行有上限的短暂等待，避免瞬时背压立即截断流，
        // 又不会无限阻塞 SDK 回调。返回 false 表示必须以明确的 queue_overflow 原因结束会话。
        public bool TryEnqueue(byte[] data, int backpressureMs)
        {
            lock (_sync)
            {
                if (_completed || data == null || data.Length == 0 || data.Length > _capacity) return false;
                var deadline = Environment.TickCount + backpressureMs;
                while (!_completed && _bytes + data.Length > _capacity)
                {
                    Interlocked.Increment(ref _backpressureEvents);
                    var remaining = deadline - Environment.TickCount;
                    if (remaining <= 0 || !System.Threading.Monitor.Wait(_sync, remaining)) return false;
                }
                if (_completed) return false;
                _items.Enqueue(data);
                _bytes += data.Length;
                if (_bytes > _peakBytes) _peakBytes = _bytes;
                System.Threading.Monitor.PulseAll(_sync);
                return true;
            }
        }

        public byte[] Dequeue(int timeoutMs)
        {
            lock (_sync)
            {
                var deadline = Environment.TickCount + timeoutMs;
                while (_items.Count == 0 && !_completed)
                {
                    var remaining = deadline - Environment.TickCount;
                    if (remaining <= 0 || !System.Threading.Monitor.Wait(_sync, remaining)) return null;
                }
                if (_items.Count == 0) return null;
                var item = _items.Dequeue();
                _bytes -= item.Length;
                System.Threading.Monitor.PulseAll(_sync);
                return item;
            }
        }

        public bool IsCompleted { get { lock (_sync) return _completed && _items.Count == 0; } }
        public long CurrentBytes { get { lock (_sync) return _bytes; } }
        public long PeakBytes { get { lock (_sync) return _peakBytes; } }
        public long BackpressureEvents { get { return Interlocked.Read(ref _backpressureEvents); } }
        public void Complete() { lock (_sync) { _completed = true; System.Threading.Monitor.PulseAll(_sync); } }
        public void Dispose() { Complete(); }
    }
}
