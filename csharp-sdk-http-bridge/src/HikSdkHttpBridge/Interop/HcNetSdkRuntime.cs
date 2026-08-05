using System;
using System.IO;

namespace HikSdkHttpBridge.Interop
{
    internal sealed class HcNetSdkRuntime : IDisposable
    {
        private readonly ExceptionCallback _exceptionCallback;
        private bool _initialized;

        public HcNetSdkRuntime() { _exceptionCallback = OnException; }
        public bool IsInitialized { get { return _initialized; } }

        public void Initialize(string directory)
        {
            if (_initialized) return;
            Log.Info("开始初始化 HCNetSDK：目录=" + directory + "，进程位数=" + (Environment.Is64BitProcess ? "x64" : "x86") + "。");
            if (Environment.Is64BitProcess) throw new BridgeException(500, "ARCH_MISMATCH", "HCNetSDK Win32 requires an x86 process");
            if (!File.Exists(Path.Combine(directory, "HCNetSDK.dll"))) throw new BridgeException(500, "SDK_NOT_FOUND", "HCNetSDK.dll not found");
            if (!NativeMethods.SetDllDirectory(directory)) throw new BridgeException(500, "SDK_LOAD_FAILED", "SetDllDirectory failed: " + System.Runtime.InteropServices.Marshal.GetLastWin32Error());
            if (!NativeMethods.NET_DVR_Init()) throw new BridgeException(500, "SDK_INIT_FAILED", "NET_DVR_Init failed: " + NativeMethods.NET_DVR_GetLastError());
            _initialized = true;
            NativeMethods.NET_DVR_SetConnectTime(3000, 1);
            NativeMethods.NET_DVR_SetReconnect(10000, 1);
            NativeMethods.NET_DVR_SetExceptionCallBack_V30(0, IntPtr.Zero, _exceptionCallback, IntPtr.Zero);
            Log.Info("HCNetSDK 初始化成功：版本=0x" + NativeMethods.NET_DVR_GetSDKBuildVersion().ToString("X8") + "，连接超时=3000ms，SDK 重连间隔=10000ms。");
        }

        private static void OnException(uint type, int userId, int handle, IntPtr user)
        {
            Log.Warn(string.Format("HCNetSDK 回调异常：类型={0}，用户={1}，句柄={2}，错误={3}。", type, userId, handle, NativeMethods.NET_DVR_GetLastError()));
        }

        public void Dispose()
        {
            if (_initialized)
            {
                Log.Info("开始清理 HCNetSDK 资源。");
                NativeMethods.NET_DVR_Cleanup();
                _initialized = false;
                Log.Info("HCNetSDK 资源清理完成。");
            }
        }
    }
}
