using System;
using System.Collections.Concurrent;
using System.Threading;

namespace HikSdkHttpBridge.Sessions
{
    internal sealed class SessionRegistry
    {
        private readonly SemaphoreSlim _slots;
        private readonly ConcurrentDictionary<string, byte> _ids = new ConcurrentDictionary<string, byte>(StringComparer.OrdinalIgnoreCase);

        public SessionRegistry(int maximum) { _slots = new SemaphoreSlim(maximum, maximum); }

        public IDisposable Reserve(string sid, bool allowDuplicate = false)
        {
            if (!_slots.Wait(0)) throw new BridgeException(503, "SESSION_LIMIT", "maximum concurrent session count reached");
            var key = allowDuplicate ? sid + "#retry-" + Guid.NewGuid().ToString("N") : sid;
            if (!_ids.TryAdd(key, 0)) { _slots.Release(); throw new BridgeException(409, "DUPLICATE_SID", "sid is already active"); }
            return new Reservation(this, key);
        }

        public int Count { get { return _ids.Count; } }

        private void Release(string sid) { if (_ids.TryRemove(sid, out var ignored)) _slots.Release(); }

        private sealed class Reservation : IDisposable
        {
            private SessionRegistry _owner;
            private readonly string _sid;
            public Reservation(SessionRegistry owner, string sid) { _owner = owner; _sid = sid; }
            public void Dispose() { var owner = Interlocked.Exchange(ref _owner, null); if (owner != null) owner.Release(_sid); }
        }
    }
}
