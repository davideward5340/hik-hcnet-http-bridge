"""Real SDK initialization/HTTP smoke; Linux additionally verifies SIGTERM.

No device credentials or NVR traffic are required.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument('--exe', type=Path, required=True)
parser.add_argument('--sdk', type=Path, required=True)
parser.add_argument('--ffmpeg', type=Path, required=True)
parser.add_argument('--config', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
config = json.loads(args.config.read_text(encoding='utf-8-sig'))
config['sdk']['directory'] = str(args.sdk.resolve())
config['ffmpeg']['path'] = str(args.ffmpeg.resolve())
config['ffmpeg']['hardwareAcceleration'] = 'off'
config['logging']['directory'] = str((args.output / 'logs').resolve())
config['media']['codecCacheFile'] = str((args.output / 'state.tsv').resolve())
with socket.socket() as probe:
    probe.bind(('127.0.0.1', 0))
    port = probe.getsockname()[1]
config['server']['port'] = port
config_path = args.output / '配置 with spaces.json'
config_path.write_text(json.dumps(config, ensure_ascii=False), encoding='utf-8')
env = os.environ.copy()
for name in ('HIK_BRIDGE_SDK_DIR', 'HIK_BRIDGE_FFMPEG_PATH', 'HIK_BRIDGE_LOG_DIR'):
    env.pop(name, None)
if os.name != 'nt':
    env['LD_LIBRARY_PATH'] = str(args.sdk.resolve()) + ':' + str(args.sdk.resolve() / 'HCNetSDKCom')
command = [str(args.exe.resolve()), '--config', str(config_path.resolve())]
flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
subprocess.run(command + ['--validate-config'], env=env, creationflags=flags, check=True, timeout=15)
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
for cycle in range(2):
    client = None
    with (args.output / f'process-{cycle}.log').open('wb') as log:
        process = subprocess.Popen(command, env=env, stdout=log, stderr=log, creationflags=flags)
        try:
            deadline = time.monotonic() + 30
            while True:
                if process.poll() is not None:
                    raise RuntimeError(f'bridge exited: {process.returncode}; see {log.name}')
                try:
                    with opener.open(f'http://127.0.0.1:{port}/healthz', timeout=1) as response:
                        health = json.load(response)
                    assert health['status'] == 'ok' and health['sdk'] == 'hcnetsdk'
                    assert health['activeSessions'] == 0
                    assert health['maxSessions'] == config['server']['maxSessions']
                    assert health['availableSessions'] == health['maxSessions']
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.1)
            client = socket.create_connection(('127.0.0.1', port))
            client.sendall(b'GET /healthz HTTP/1.1\r\n')
            if os.name == 'nt':
                # Windows graceful stop is tested by lifecycle + elevated SCM tests.
                process.terminate()
            else:
                process.send_signal(signal.SIGTERM)
            code = process.wait(timeout=10)
            if os.name != 'nt':
                assert code == 0, f'SIGTERM exit code: {code}'
            print(f'PASS cycle {cycle + 1}: SDK initialization, HTTP, partial request, process exit')
        finally:
            if client:
                client.close()
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
print('PASS runtime smoke; Windows termination is forced, Linux termination is graceful')
