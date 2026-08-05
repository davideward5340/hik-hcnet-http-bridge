using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Web.Script.Serialization;
using HikSdkHttpBridge.Model;

namespace HikSdkHttpBridge.Media
{
    // 不保存口令。缓存仅用于跳过重复的编码探测，不影响每次请求独立登录和授权。
    // 除内存命中外，将未过期的通道编码写入本地状态文件，使服务重启后的首次访问也可直接选用正确管线。
    internal sealed class VideoCodecCache
    {
        private sealed class Entry
        {
            public DetectedVideoCodec Codec { get; set; }
            public DateTime ExpiresUtc { get; set; }
        }

        private sealed class PersistedEntry
        {
            public string Codec { get; set; }
            public long ExpiresUtcTicks { get; set; }
        }

        private readonly int _ttlSeconds;
        private readonly string _filePath;
        private readonly object _persistenceLock = new object();
        private readonly ConcurrentDictionary<string, Entry> _entries = new ConcurrentDictionary<string, Entry>(StringComparer.Ordinal);

        public VideoCodecCache(int ttlSeconds, string filePath)
        {
            _ttlSeconds = ttlSeconds;
            _filePath = filePath ?? string.Empty;
            LoadPersistedEntries();
        }

        public bool TryGet(StreamRequest request, out DetectedVideoCodec codec)
        {
            codec = DetectedVideoCodec.Unknown;
            if (_ttlSeconds <= 0) return false;
            Entry entry;
            var key = BuildKey(request);
            if (!_entries.TryGetValue(key, out entry)) return false;
            if (entry.ExpiresUtc <= DateTime.UtcNow)
            {
                Entry ignored;
                _entries.TryRemove(key, out ignored);
                PersistQuietly();
                return false;
            }
            codec = entry.Codec;
            return codec == DetectedVideoCodec.H264 || codec == DetectedVideoCodec.H265;
        }

        public void Set(StreamRequest request, DetectedVideoCodec codec)
        {
            if (_ttlSeconds <= 0 || (codec != DetectedVideoCodec.H264 && codec != DetectedVideoCodec.H265)) return;
            _entries[BuildKey(request)] = new Entry { Codec = codec, ExpiresUtc = DateTime.UtcNow.AddSeconds(_ttlSeconds) };
            PersistQuietly();
        }

        private void LoadPersistedEntries()
        {
            if (_ttlSeconds <= 0 || string.IsNullOrWhiteSpace(_filePath) || !File.Exists(_filePath)) return;
            try
            {
                var content = File.ReadAllText(_filePath);
                var values = new JavaScriptSerializer().Deserialize<Dictionary<string, PersistedEntry>>(content);
                if (values == null) return;
                var now = DateTime.UtcNow;
                var count = 0;
                foreach (var item in values)
                {
                    if (item.Value == null) continue;
                    var codec = ParseCodec(item.Value.Codec);
                    if (codec == DetectedVideoCodec.Unknown) continue;
                    var expires = new DateTime(item.Value.ExpiresUtcTicks, DateTimeKind.Utc);
                    if (expires <= now) continue;
                    _entries[item.Key] = new Entry { Codec = codec, ExpiresUtc = expires };
                    count++;
                }
                if (count > 0) Log.Info("已加载持久化视频编码缓存：条目=" + count + "。");
            }
            catch (Exception ex)
            {
                Log.Warn("无法读取持久化视频编码缓存，将按设备配置或实际码流重新识别：" + ex.GetType().Name + "。");
            }
        }

        private void PersistQuietly()
        {
            if (_ttlSeconds <= 0 || string.IsNullOrWhiteSpace(_filePath)) return;
            lock (_persistenceLock)
            {
                try
                {
                    var now = DateTime.UtcNow;
                    var values = new Dictionary<string, PersistedEntry>(StringComparer.Ordinal);
                    foreach (var item in _entries)
                    {
                        if (item.Value.ExpiresUtc <= now) continue;
                        values[item.Key] = new PersistedEntry
                        {
                            Codec = item.Value.Codec == DetectedVideoCodec.H264 ? "h264" : "h265",
                            ExpiresUtcTicks = item.Value.ExpiresUtc.Ticks
                        };
                    }
                    var directory = Path.GetDirectoryName(_filePath);
                    if (!string.IsNullOrWhiteSpace(directory)) Directory.CreateDirectory(directory);
                    var temporary = _filePath + ".tmp";
                    File.WriteAllText(temporary, new JavaScriptSerializer().Serialize(values));
                    if (File.Exists(_filePath)) File.Delete(_filePath);
                    File.Move(temporary, _filePath);
                }
                catch (Exception ex)
                {
                    Log.Warn("无法保存持久化视频编码缓存，将继续使用内存缓存：" + ex.GetType().Name + "。");
                }
            }
        }

        private static DetectedVideoCodec ParseCodec(string value)
        {
            if (string.Equals(value, "h264", StringComparison.OrdinalIgnoreCase)) return DetectedVideoCodec.H264;
            if (string.Equals(value, "h265", StringComparison.OrdinalIgnoreCase)) return DetectedVideoCodec.H265;
            return DetectedVideoCodec.Unknown;
        }

        private static string BuildKey(StreamRequest request)
        {
            return (request.Ip ?? string.Empty).Trim().ToLowerInvariant() + "\n" + request.Port + "\n" + request.Camera + "\n" + request.ChannelType + "\n" + request.Stream;
        }
    }
}
