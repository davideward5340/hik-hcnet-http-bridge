using System;

namespace HikSdkHttpBridge
{
    internal sealed class BridgeException : Exception
    {
        public BridgeException(int httpStatus, string code, string message) : base(message) { HttpStatus = httpStatus; Code = code; }
        public int HttpStatus { get; private set; }
        public string Code { get; private set; }
    }
}
