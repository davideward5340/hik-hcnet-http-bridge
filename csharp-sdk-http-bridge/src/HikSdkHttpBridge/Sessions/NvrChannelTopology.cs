using System;
using System.Collections.Generic;
using HikSdkHttpBridge.Interop;
using HikSdkHttpBridge.Model;

namespace HikSdkHttpBridge.Sessions
{
    internal enum ResolvedChannelType { Analog, Digital, Unknown }

    // 将 SDK 的设备通道能力集中在一个对象中处理，避免前端用固定 +32 猜测。
    internal sealed class NvrChannelTopology
    {
        private const uint GetIpParaCfgV40 = 1062; // HCNetSDK.h: NET_DVR_GET_IPPARACFG_V40
        private readonly bool[] _analogEnabled = new bool[64];

        public int AnalogStartChannel { get; private set; }
        public int AnalogChannelCount { get; private set; }
        public int DigitalStartChannel { get; private set; }
        public int DigitalChannelCount { get; private set; }
        public bool IpParameterConfigRead { get; private set; }

        public static NvrChannelTopology Read(int userId, NetDvrDeviceInfoV40 deviceInfo, string sid)
        {
            var device = deviceInfo.DeviceV30;
            var topology = new NvrChannelTopology
            {
                AnalogStartChannel = device.AnalogStartChannel,
                AnalogChannelCount = device.AnalogChannelCount,
                DigitalStartChannel = device.DigitalStartChannel,
                DigitalChannelCount = device.IpChannelCountLow | (device.IpChannelCountHigh << 8)
            };

            for (var index = 0; index < topology._analogEnabled.Length; index++)
                topology._analogEnabled[index] = index < topology.AnalogChannelCount;

            var ipConfig = NetDvrIpParaCfgV40.Create();
            uint bytesReturned;
            if (NativeMethods.NET_DVR_GetDVRConfig(userId, GetIpParaCfgV40, 0, ref ipConfig, ipConfig.Size, out bytesReturned))
            {
                topology.IpParameterConfigRead = true;
                // 少数旧 DVR 虽支持 V40 查询，但该结构会把模拟数量返回为 0；
                // 不能因此覆盖登录信息中的真实模拟能力。
                if (ipConfig.AnalogChannelCount > 0 && ipConfig.AnalogChannelCount <= int.MaxValue) topology.AnalogChannelCount = Math.Max(topology.AnalogChannelCount, (int)ipConfig.AnalogChannelCount);
                if (ipConfig.DigitalChannelCount > 0 && ipConfig.DigitalChannelCount <= int.MaxValue) topology.DigitalChannelCount = Math.Max(topology.DigitalChannelCount, (int)ipConfig.DigitalChannelCount);
                if (ipConfig.DigitalStartChannel <= int.MaxValue && ipConfig.DigitalStartChannel > 0) topology.DigitalStartChannel = (int)ipConfig.DigitalStartChannel;
                if (ipConfig.AnalogChannelCount > 0)
                {
                    for (var index = 0; index < topology._analogEnabled.Length; index++)
                        topology._analogEnabled[index] = ipConfig.AnalogChannelEnabled[index] != 0;
                }
            }
            else
            {
                Log.Warn("sid=" + sid + " 未读取到 NVR IP 通道配置，按登录返回的通道能力继续：HCNetSDK 错误=" + NativeMethods.NET_DVR_GetLastError() + "。");
            }

            Log.Info("sid=" + sid + " NVR 通道能力已读取：模拟起始/数量=" + topology.AnalogStartChannel + "/" + topology.AnalogChannelCount + "，数字起始/数量=" + topology.DigitalStartChannel + "/" + topology.DigitalChannelCount + "。");
            return topology;
        }

        // 返回真正交给 HCNetSDK 的通道号。digitalIndex 使用本项目后台的零基序号：
        // 真实通道 = dwStartDChan + 索引，例如起始 33、索引 0 对应真实通道 33。
        public int Resolve(int camera, ChannelType requestedType, out ResolvedChannelType resolvedType)
        {
            var analog = IsAnalog(camera);
            var digital = IsDigital(camera);

            if (requestedType == ChannelType.Analog)
            {
                if (AnalogChannelCount <= 0) throw UnsupportedAnalog(camera);
                if (!analog) throw new BridgeException(422, "ANALOG_CHANNEL_OUT_OF_RANGE", "camera=" + camera + " is not in the NVR analog channel range " + ChannelRange(AnalogStartChannel, AnalogChannelCount));
                EnsureAnalogEnabled(camera);
                resolvedType = ResolvedChannelType.Analog;
                return camera;
            }
            if (requestedType == ChannelType.Digital)
            {
                if (DigitalChannelCount <= 0) throw new BridgeException(422, "DIGITAL_CHANNEL_UNSUPPORTED", "NVR reports no digital/IP channels");
                if (!digital) throw new BridgeException(422, "DIGITAL_CHANNEL_OUT_OF_RANGE", "camera=" + camera + " is not in the NVR digital/IP channel range " + ChannelRange(DigitalStartChannel, DigitalChannelCount));
                resolvedType = ResolvedChannelType.Digital;
                return camera;
            }
            if (requestedType == ChannelType.DigitalIndex)
            {
                if (DigitalChannelCount <= 0) throw new BridgeException(422, "DIGITAL_CHANNEL_UNSUPPORTED", "NVR reports no digital/IP channels");
                if (camera < 0 || camera >= DigitalChannelCount) throw new BridgeException(422, "DIGITAL_CHANNEL_INDEX_OUT_OF_RANGE", "camera=" + camera + " is not in the zero-based digital channel index range 0-" + (DigitalChannelCount - 1));
                resolvedType = ResolvedChannelType.Digital;
                return DigitalStartChannel + camera;
            }

            if (analog)
            {
                EnsureAnalogEnabled(camera);
                resolvedType = ResolvedChannelType.Analog;
                return camera;
            }
            if (digital)
            {
                resolvedType = ResolvedChannelType.Digital;
                return camera;
            }

            throw new BridgeException(422, "CHANNEL_NOT_FOUND", "camera=" + camera + " is not reported by this NVR; analog=" + ChannelRange(AnalogStartChannel, AnalogChannelCount) + ", digital=" + ChannelRange(DigitalStartChannel, DigitalChannelCount));
        }

        private bool IsAnalog(int camera)
        {
            return AnalogChannelCount > 0 && camera >= AnalogStartChannel && camera < AnalogStartChannel + AnalogChannelCount;
        }

        private bool IsDigital(int camera)
        {
            return DigitalChannelCount > 0 && camera >= DigitalStartChannel && camera < DigitalStartChannel + DigitalChannelCount;
        }

        private void EnsureAnalogEnabled(int camera)
        {
            var index = camera - AnalogStartChannel;
            if (IpParameterConfigRead && (index < 0 || index >= _analogEnabled.Length || !_analogEnabled[index]))
                throw new BridgeException(422, "ANALOG_CHANNEL_DISABLED", "camera=" + camera + " is an analog channel but is disabled or has no analog input configured on the NVR");
        }

        private BridgeException UnsupportedAnalog(int camera)
        {
            return new BridgeException(422, "ANALOG_CHANNEL_UNSUPPORTED", "camera=" + camera + " requested as analog, but this NVR reports no analog channels; use the NVR digital/IP SDK channel number (often starting at " + (DigitalStartChannel > 0 ? DigitalStartChannel.ToString() : "33") + ") for an IP camera");
        }

        private int EnabledAnalogChannelCount()
        {
            var enabled = 0;
            for (var index = 0; index < AnalogChannelCount && index < _analogEnabled.Length; index++) if (_analogEnabled[index]) enabled++;
            return enabled;
        }

        private static string ChannelRange(int start, int count)
        {
            return count <= 0 || start <= 0 ? "none" : start + "-" + (start + count - 1);
        }
    }
}
