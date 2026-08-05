using System;
using System.Runtime.InteropServices;

namespace HikSdkHttpBridge.Interop
{
    internal static class NativeMethods
    {
        internal const uint SystemHeader = 1;
        internal const uint StreamData = 2;
        internal const uint StandardVideoData = 4;
        internal const uint PlayStart = 1;
        internal const uint PlayFast = 5;
        internal const uint PlaySlow = 6;
        internal const uint PlayNormal = 7;
        internal const uint PlayKeepAlive = 25;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool SetDllDirectory(string path);

        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool NET_DVR_Init();
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool NET_DVR_Cleanup();
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool NET_DVR_SetConnectTime(uint timeout, uint retryCount);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool NET_DVR_SetReconnect(uint interval, int enableReconnect);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool NET_DVR_SetExceptionCallBack_V30(uint message, IntPtr window, ExceptionCallback callback, IntPtr user);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        internal static extern uint NET_DVR_GetLastError();
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        internal static extern uint NET_DVR_GetSDKBuildVersion();
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        internal static extern int NET_DVR_Login_V40(ref NetDvrUserLoginInfo loginInfo, out NetDvrDeviceInfoV40 deviceInfo);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool NET_DVR_Logout(int userId);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool NET_DVR_GetDVRConfig(int userId, uint command, int channel, ref NetDvrIpParaCfgV40 output, uint outputSize, out uint bytesReturned);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool NET_DVR_GetDVRConfig(int userId, uint command, int channel, ref NetDvrCompressionCfgV30 output, uint outputSize, out uint bytesReturned);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool NET_DVR_SetDVRConfig(int userId, uint command, int channel, ref NetDvrCompressionCfgV30 input, uint inputSize);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        internal static extern int NET_DVR_RealPlay_V40(int userId, ref NetDvrPreviewInfo previewInfo, RealDataCallback callback, IntPtr user);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool NET_DVR_StopRealPlay(int realHandle);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool NET_DVR_MakeKeyFrame(int userId, int channel);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool NET_DVR_MakeKeyFrameSub(int userId, int channel);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        internal static extern int NET_DVR_PlayBackByTime_V40(int userId, ref NetDvrVodPara vodPara);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool NET_DVR_SetPlayDataCallBack_V40(int playHandle, PlayDataCallback callback, IntPtr user);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool NET_DVR_PlayBackControl_V40(int playHandle, uint command, IntPtr input, uint inputLength, IntPtr output, IntPtr outputLength);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        internal static extern int NET_DVR_GetPlayBackPos(int playHandle);
        [DllImport("HCNetSDK.dll", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)] internal static extern bool NET_DVR_StopPlayBack(int playHandle);
    }
}
