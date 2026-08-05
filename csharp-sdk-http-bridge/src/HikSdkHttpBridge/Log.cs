using System;
using System.Globalization;
using System.IO;
using System.Text;
using HikSdkHttpBridge.Config;

namespace HikSdkHttpBridge
{
    internal static class Log
    {
        private static readonly object Sync = new object();
        private static string _directory;
        private static DateTime _activeDate = DateTime.MinValue;
        private static int _retentionDays = 30;

        public static void Initialize(LoggingConfig config)
        {
            lock (Sync)
            {
                _directory = config.Directory;
                _retentionDays = config.RetentionDays < 1 ? 30 : config.RetentionDays;
                Directory.CreateDirectory(_directory);
                EnsureDailyFile(DateTime.Now);
            }
        }

        public static void Info(string message) { Write("信息", message); }
        public static void Warn(string message) { Write("警告", message); }
        public static void Error(string message) { Write("错误", message); }

        private static void Write(string level, string message)
        {
            var now = DateTime.Now;
            var line = string.Format("{0:yyyy-MM-dd HH:mm:ss.fff} [{1}] {2}", now, level, message);
            lock (Sync)
            {
                Console.WriteLine(line);
                if (string.IsNullOrEmpty(_directory)) return;
                try
                {
                    var file = EnsureDailyFile(now);
                    File.AppendAllText(file, line + Environment.NewLine, Encoding.UTF8);
                }
                catch
                {
                    // 日志故障不能影响取流服务；控制台输出仍保留给服务管理器或启动脚本。
                }
            }
        }

        private static string EnsureDailyFile(DateTime now)
        {
            var date = now.Date;
            if (_activeDate != date)
            {
                Directory.CreateDirectory(_directory);
                DeleteExpiredDailyLogs(date);
                _activeDate = date;
            }
            return Path.Combine(_directory, date.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture) + ".log");
        }

        private static void DeleteExpiredDailyLogs(DateTime today)
        {
            var cutoff = today.AddDays(-_retentionDays);
            foreach (var path in Directory.EnumerateFiles(_directory, "*.log", SearchOption.TopDirectoryOnly))
            {
                try
                {
                    var name = Path.GetFileNameWithoutExtension(path);
                    DateTime date;
                    var isDailyLog = DateTime.TryParseExact(name, "yyyy-MM-dd", CultureInfo.InvariantCulture, DateTimeStyles.None, out date);
                    if ((isDailyLog && date < cutoff) || (!isDailyLog && File.GetLastWriteTime(path).Date < cutoff))
                    {
                        File.Delete(path);
                    }
                }
                catch
                {
                    // 锁定、权限不足或异常命名的文件保留到下次清理，不中断服务。
                }
            }
        }
    }
}
