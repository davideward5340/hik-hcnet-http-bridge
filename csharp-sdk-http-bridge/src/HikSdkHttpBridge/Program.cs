using System;
using System.IO;
using System.ServiceProcess;
using System.Threading;
using HikSdkHttpBridge.Config;
using HikSdkHttpBridge.Http;

namespace HikSdkHttpBridge
{
    internal static class Program
    {
        private const string Version = "1.0.0";

        private static int Main(string[] args)
        {
            try
            {
                var command = args.Length == 0 ? "run" : args[0].ToLowerInvariant();
                if (command == "version" || command == "--version") { Console.WriteLine("hik-sdk-http-bridge " + Version); return 0; }
                var configPath = FindConfig(args);
                var config = AppConfig.Load(configPath);
                config.ResolvePaths(AppDomain.CurrentDomain.BaseDirectory);
                ValidateRuntimeFiles(config);
                Log.Initialize(config.Logging);
                if (command == "validate-config") { Console.WriteLine("配置校验通过：" + configPath); return 0; }
                if (command == "service-run") { ServiceBase.Run(new BridgeWindowsService(config)); return 0; }
                if (command != "run") throw new ConfigException("INVALID_COMMAND", "command must be run, service-run, validate-config, or version");
                return RunConsole(config);
            }
            catch (ConfigException ex) { Console.Error.WriteLine("配置错误：错误码=" + ex.Code + "。"); return 2; }
            catch (BridgeException ex) { Console.Error.WriteLine("桥接服务错误：错误码=" + ex.Code + "。"); return 3; }
            catch (Exception ex) { Console.Error.WriteLine("程序发生未处理异常：类型=" + ex.GetType().FullName + "，HRESULT=" + ex.HResult + "。"); return 1; }
        }

        private static int RunConsole(AppConfig config)
        {
            using (var server = new HttpBridgeServer(config))
            using (var stopped = new ManualResetEventSlim(false))
            {
                Console.CancelKeyPress += (sender, e) => { e.Cancel = true; stopped.Set(); };
                server.Start();
                Console.WriteLine("服务正在运行，按 Ctrl+C 停止。");
                stopped.Wait();
            }
            return 0;
        }

        private static string FindConfig(string[] args)
        {
            for (var i = 0; i < args.Length; i++)
            {
                if (args[i].Equals("--config", StringComparison.OrdinalIgnoreCase))
                {
                    if (i + 1 >= args.Length) throw new ConfigException("INVALID_COMMAND", "--config requires a path");
                    return Path.GetFullPath(args[i + 1]);
                }
            }
            return Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "config.json");
        }

        private static void ValidateRuntimeFiles(AppConfig config)
        {
            if (!File.Exists(Path.Combine(config.Sdk.HcnetDirectory, "HCNetSDK.dll"))) throw new ConfigException("SDK_NOT_FOUND", "HCNetSDK.dll not found: " + config.Sdk.HcnetDirectory);
            if (!File.Exists(config.Ffmpeg.Path)) throw new ConfigException("FFMPEG_NOT_FOUND", "ffmpeg.exe not found: " + config.Ffmpeg.Path);
        }
    }

    internal sealed class BridgeWindowsService : ServiceBase
    {
        private readonly AppConfig _config;
        private HttpBridgeServer _server;

        public BridgeWindowsService(AppConfig config)
        {
            _config = config;
            ServiceName = "hikbridge";
            CanStop = true;
            AutoLog = true;
        }

        protected override void OnStart(string[] args)
        {
            _server = new HttpBridgeServer(_config);
            _server.Start();
        }

        protected override void OnStop()
        {
            if (_server != null) { _server.Dispose(); _server = null; }
        }
    }
}
