using System;
using System.Runtime.InteropServices;

namespace HikSdkHttpBridge.Interop
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct NetDvrUserLoginInfo
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 129)] public byte[] DeviceAddress;
        public byte UseTransport;
        public ushort Port;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public byte[] UserName;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public byte[] Password;
        public IntPtr LoginResultCallback;
        public IntPtr User;
        public int UseAsyncLogin;
        public byte ProxyType;
        public byte UseUtcTime;
        public byte LoginMode;
        public byte Https;
        public int ProxyId;
        public byte VerifyMode;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 119)] public byte[] Reserved;

        public static NetDvrUserLoginInfo Create()
        {
            return new NetDvrUserLoginInfo { DeviceAddress = new byte[129], UserName = new byte[64], Password = new byte[64], Reserved = new byte[119] };
        }
    }

    // HCNetSDK.h: NET_DVR_DEVICEINFO_V30。登录返回的 byChanNum/byStartChan
    // 是模拟通道数量/起始号；byStartDChan 与 byHighDChanNum 用于数字/IP 通道。
    [StructLayout(LayoutKind.Sequential)]
    internal struct NetDvrDeviceInfoV30
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 48)] public byte[] SerialNumber;
        public byte AlarmInPortCount;
        public byte AlarmOutPortCount;
        public byte DiskCount;
        public byte DeviceType;
        public byte AnalogChannelCount;
        public byte AnalogStartChannel;
        public byte AudioChannelCount;
        public byte IpChannelCountLow;
        public byte ZeroChannelCount;
        public byte MainProtocol;
        public byte SubProtocol;
        public byte Support;
        public byte Support1;
        public byte Support2;
        public ushort DeviceModel;
        public byte Support3;
        public byte MultiStreamProtocol;
        public byte DigitalStartChannel;
        public byte DigitalTalkStartChannel;
        public byte IpChannelCountHigh;
        public byte Support4;
        public byte LanguageType;
        public byte VoiceInChannelCount;
        public byte VoiceInStartChannel;
        public byte Support5;
        public byte Support6;
        public byte MirrorChannelCount;
        public ushort MirrorStartChannel;
        public byte Support7;
        public byte Reserved;

        public static NetDvrDeviceInfoV30 Create()
        {
            return new NetDvrDeviceInfoV30 { SerialNumber = new byte[48] };
        }
    }

    // HCNetSDK.h: NET_DVR_DEVICEINFO_V40 的后续字段在本项目中不参与通道识别，
    // 但必须保留完整的 344 字节 ABI，避免 SDK 写入越界。
    [StructLayout(LayoutKind.Sequential)]
    internal struct NetDvrDeviceInfoV40
    {
        public NetDvrDeviceInfoV30 DeviceV30;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 264)] public byte[] Reserved;

        public static NetDvrDeviceInfoV40 Create()
        {
            return new NetDvrDeviceInfoV40 { DeviceV30 = NetDvrDeviceInfoV30.Create(), Reserved = new byte[264] };
        }
    }

    // HCNetSDK.h: NET_DVR_IPPARACFG_V40。仅消费其前部的通道数量、起始数字
    // 通道和模拟通道启用位；其余区域使用精确长度的字节数组保留 ABI。
    [StructLayout(LayoutKind.Sequential)]
    internal struct NetDvrIpParaCfgV40
    {
        public uint Size;
        public uint GroupCount;
        public uint AnalogChannelCount;
        public uint DigitalChannelCount;
        public uint DigitalStartChannel;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64)] public byte[] AnalogChannelEnabled;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64 * 296)] public byte[] IpDeviceInfo;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 64 * 504)] public byte[] StreamModes;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 20)] public byte[] Reserved;

        public static NetDvrIpParaCfgV40 Create()
        {
            var value = new NetDvrIpParaCfgV40
            {
                AnalogChannelEnabled = new byte[64],
                IpDeviceInfo = new byte[64 * 296],
                StreamModes = new byte[64 * 504],
                Reserved = new byte[20]
            };
            value.Size = (uint)Marshal.SizeOf(typeof(NetDvrIpParaCfgV40));
            return value;
        }
    }

    // HCNetSDK.h: NET_DVR_COMPRESSION_INFO_V30 / NET_DVR_COMPRESSIONCFG_V30。
    // 仅读取编码类型与 I 帧间隔，但必须保留完整 28/116 字节 ABI 才能安全调用 Get/SetDVRConfig。
    [StructLayout(LayoutKind.Sequential)]
    internal struct NetDvrCompressionInfoV30
    {
        public byte StreamType;
        public byte Resolution;
        public byte BitrateType;
        public byte PictureQuality;
        public uint VideoBitrate;
        public uint VideoFrameRate;
        public ushort IntervalFrameI;
        public byte IntervalBpFrame;
        public byte Reserved1;
        public byte VideoEncodingType;
        public byte AudioEncodingType;
        public byte VideoEncodingComplexity;
        public byte EnableSvc;
        public byte FormatType;
        public byte AudioBitrate;
        public byte StreamSmooth;
        public byte AudioSamplingRate;
        public byte SmartCodec;
        public byte DepthMapEnable;
        public ushort AverageVideoBitrate;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NetDvrCompressionCfgV30
    {
        public uint Size;
        public NetDvrCompressionInfoV30 NormalHighRecord;
        public NetDvrCompressionInfoV30 Reserved;
        public NetDvrCompressionInfoV30 EventRecord;
        public NetDvrCompressionInfoV30 Network;

        public static NetDvrCompressionCfgV30 Create()
        {
            return new NetDvrCompressionCfgV30 { Size = (uint)Marshal.SizeOf(typeof(NetDvrCompressionCfgV30)) };
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NetDvrPreviewInfo
    {
        public int Channel;
        public uint StreamType;
        public uint LinkMode;
        public IntPtr PlayWindow;
        public uint Blocked;
        public uint PassbackRecord;
        public byte PreviewMode;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public byte[] StreamId;
        public byte ProtocolType;
        public byte Reserved1;
        public byte VideoCodingType;
        public uint DisplayBufferCount;
        public byte NpqMode;
        public byte ReceiveMetadata;
        public byte DataType;
        public byte Reconnect;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 212)] public byte[] Reserved;

        public static NetDvrPreviewInfo Create()
        {
            return new NetDvrPreviewInfo { StreamId = new byte[32], Reserved = new byte[212] };
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NetDvrTime
    {
        public uint Year, Month, Day, Hour, Minute, Second;
        public static NetDvrTime FromUnixSeconds(long seconds)
        {
            var value = DateTimeOffset.FromUnixTimeSeconds(seconds).LocalDateTime;
            return new NetDvrTime { Year = (uint)value.Year, Month = (uint)value.Month, Day = (uint)value.Day, Hour = (uint)value.Hour, Minute = (uint)value.Minute, Second = (uint)value.Second };
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NetDvrStreamInfo
    {
        public uint Size;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public byte[] Id;
        public uint Channel;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public byte[] Reserved;
        public static NetDvrStreamInfo Create(int channel)
        {
            return new NetDvrStreamInfo { Size = (uint)Marshal.SizeOf(typeof(NetDvrStreamInfo)), Id = new byte[32], Channel = (uint)channel, Reserved = new byte[32] };
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NetDvrVodPara
    {
        public uint Size;
        public NetDvrStreamInfo StreamInfo;
        public NetDvrTime BeginTime;
        public NetDvrTime EndTime;
        public IntPtr Window;
        public byte DrawFrame, VolumeType, VolumeNumber, StreamType;
        public uint FileIndex;
        public byte AudioFile, CourseFile, Download, OptimalStreamType, UseAsync;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 19)] public byte[] Reserved;
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    internal delegate void RealDataCallback(int handle, uint dataType, IntPtr buffer, uint size, IntPtr user);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    internal delegate void PlayDataCallback(int handle, uint dataType, IntPtr buffer, uint size, IntPtr user);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    internal delegate void ExceptionCallback(uint type, int userId, int handle, IntPtr user);
}
