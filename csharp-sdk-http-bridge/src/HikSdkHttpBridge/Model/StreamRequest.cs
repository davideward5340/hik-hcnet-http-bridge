using System;
using System.Collections.Specialized;
using System.Globalization;
using HikSdkHttpBridge.Config;

namespace HikSdkHttpBridge.Model
{
    internal enum StreamOption { RealPlay, Playback }
    internal enum StreamKind { Main, Sub }
    // analog/digital 的 camera 是真实 HCNetSDK 通道号；digitalIndex 的 camera 是
    // 上层存储的零基数字通道序号，由 Bridge 根据设备 dwStartDChan 动态换算。
    internal enum ChannelType { Auto, Analog, Digital, DigitalIndex }

    internal sealed class StreamRequest
    {
        private static readonly double[] Speeds = { 16, 8, 4, 2, 1, .5, .25, .125, .0625 };

        public StreamOption Option { get; private set; }
        public string Ip { get; private set; }
        public ushort Port { get; private set; }
        public string UserName { get; private set; }
        public string Password { get; private set; }
        public int Camera { get; private set; }
        public ChannelType ChannelType { get; private set; }
        public StreamKind Stream { get; private set; }
        public double Speed { get; private set; }
        public long Start { get; private set; }
        public long End { get; private set; }
        public string Sid { get; private set; }

        public static StreamRequest Parse(NameValueCollection query, AppConfig config)
        {
            var request = new StreamRequest();
            var option = Required(query, "option");
            if (option.Equals("realplay", StringComparison.OrdinalIgnoreCase)) request.Option = StreamOption.RealPlay;
            else if (option.Equals("playback", StringComparison.OrdinalIgnoreCase)) request.Option = StreamOption.Playback;
            else throw Bad("option must be realplay or playback");

            request.Ip = Required(query, "ip");
            if (request.Ip.Length > 128) throw Bad("ip is too long");
            int port;
            if (!int.TryParse(Required(query, "port"), NumberStyles.None, CultureInfo.InvariantCulture, out port) || port < 1 || port > 65535) throw Bad("port must be 1..65535");
            request.Port = (ushort)port;
            request.UserName = Required(query, "username");
            request.Password = Required(query, "password");
            if (request.UserName.Length > 63 || request.Password.Length > 63) throw Bad("username or password is too long");
            if (!int.TryParse(Required(query, "camera"), NumberStyles.None, CultureInfo.InvariantCulture, out var camera) || camera < 0) throw Bad("camera must be a non-negative integer");
            request.Camera = camera;
            var channelType = query["channelType"] ?? "auto";
            if (channelType.Equals("auto", StringComparison.OrdinalIgnoreCase)) request.ChannelType = ChannelType.Auto;
            else if (channelType.Equals("analog", StringComparison.OrdinalIgnoreCase)) request.ChannelType = ChannelType.Analog;
            else if (channelType.Equals("digital", StringComparison.OrdinalIgnoreCase) || channelType.Equals("ip", StringComparison.OrdinalIgnoreCase)) request.ChannelType = ChannelType.Digital;
            else if (channelType.Equals("digitalIndex", StringComparison.OrdinalIgnoreCase)) request.ChannelType = ChannelType.DigitalIndex;
            else throw Bad("channelType must be auto, analog, digital, digitalIndex or ip");
            if (request.ChannelType != ChannelType.DigitalIndex && request.Camera < 1) throw Bad("camera must be a positive integer unless channelType=digitalIndex");
            var stream = query["stream"] ?? "main";
            if (stream.Equals("main", StringComparison.OrdinalIgnoreCase)) request.Stream = StreamKind.Main;
            else if (stream.Equals("sub", StringComparison.OrdinalIgnoreCase)) request.Stream = StreamKind.Sub;
            else throw Bad("stream must be main or sub");
            if (!double.TryParse(query["speed"] ?? "1", NumberStyles.AllowDecimalPoint, CultureInfo.InvariantCulture, out var speed) || Array.IndexOf(Speeds, speed) < 0) throw Bad("unsupported speed");
            request.Speed = speed;
            request.Sid = Required(query, "sid");
            if (request.Sid.Length > 128 || !IsSafeSessionId(request.Sid)) throw Bad("sid must contain only letters, digits, hyphen, or underscore");

            if (request.Option == StreamOption.RealPlay)
            {
                if (request.Speed != 1) throw Bad("realplay only supports speed=1");
            }
            else
            {
                if (!long.TryParse(Required(query, "start"), NumberStyles.Integer, CultureInfo.InvariantCulture, out var start) || !long.TryParse(Required(query, "end"), NumberStyles.Integer, CultureInfo.InvariantCulture, out var end)) throw Bad("start and end must be Unix seconds");
                if (start < 0 || end <= start) throw Bad("end must be greater than start");
                if (end - start > config.Media.MaxPlaybackSeconds) throw Bad("playback duration exceeds media.maxPlaybackSeconds");
                request.Start = start;
                request.End = end;
            }
            return request;
        }

        private static string Required(NameValueCollection query, string name)
        {
            var value = query[name];
            if (string.IsNullOrWhiteSpace(value)) throw Bad(name + " is required");
            return value;
        }

        private static bool IsSafeSessionId(string value)
        {
            foreach (var character in value)
            {
                if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                    (character >= '0' && character <= '9') || character == '-' || character == '_') continue;
                return false;
            }
            return true;
        }

        private static BridgeException Bad(string message) { return new BridgeException(400, "INVALID_PARAMETER", message); }
    }
}
