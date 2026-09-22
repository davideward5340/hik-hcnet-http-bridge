"""Run real CMD parsing/control flow against native command doubles, no elevation.

All command replacements are confined to disposable package copies. The shipped
script always uses System32 tools. This does not replace actual SCM acceptance.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--stub', type=Path, required=True)
p.add_argument('--source', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
tools = a.output / 'mock-tools'
tools.mkdir(exist_ok=True)
for name in ('sc.exe', 'reg.exe', 'schtasks.exe', 'fltmc.exe', 'ping.exe'):
    shutil.copyfile(a.stub, tools / name)
content = (a.source / 'windows-service.cmd').read_bytes().decode('gbk')
assert not content.startswith('\ufeff')
assert 'powershell' not in content.lower() and 'wmic' not in content.lower()
for name in ('sc.exe', 'reg.exe', 'schtasks.exe', 'fltmc.exe', 'ping.exe'):
    content = content.replace('%SystemRoot%\\System32\\' + name, str((tools / name).resolve()))

def scenario(name, *, existing=False, task=False, marker=None, action='install', check=False):
    folder = a.output / name
    # Do not replace existing state; every invocation uses a fresh test directory.
    folder.mkdir()
    state = folder / 'state'
    state.mkdir()
    package = folder / '中文 空格 & %literal% !目录 (test)'
    package.mkdir()
    (package / 'windows-service.cmd').write_bytes(content.encode('gbk'))
    for operation, filename in (('install', '安装.bat'), ('uninstall', '卸载.bat')):
        shutil.copyfile(a.source / filename, package / filename)
    shutil.copyfile(a.source / ('安装.bat' if action == 'install' else '卸载.bat'), package / 'entry.cmd')
    shutil.copyfile(a.stub, package / 'hik-sdk-http-bridge.exe')
    (package / 'config.json').write_text('{}', encoding='utf-8')
    original = '"D:\\旧目录 & %原路径% ! 数据\\bridge.exe" service --config "D:\\旧配置\\config.json"'
    if existing:
        (state / 'hikbridge.service').write_text(original, encoding='utf-8')
        (state / 'hikbridge.state').write_text('4', encoding='ascii')
    if task:
        (state / 'HikSdkHttpBridge.task').write_text('<Task>original</Task>', encoding='ascii')
    if marker:
        (state / marker).touch()
    env = os.environ.copy()
    env['HIK_CMD_TEST_STATE'] = str(state.resolve())
    # Ensure %literal% in the directory cannot be expanded by an accidental CALL.
    env['literal'] = 'CORRUPTED_PATH'
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    # Exercise a caller starting in UTF-8, and verify that the GBK script restores
    # its caller's code page after both success and failure paths.
    driver = '@echo off\r\nchcp 65001 >nul\r\ncall entry.cmd ' + ('/check' if check else '/nopause')
    driver += '\r\nset "CASE_RESULT=%errorlevel%"\r\nchcp\r\nexit /b %CASE_RESULT%\r\n'
    (package / 'driver.cmd').write_bytes(driver.encode('ascii'))
    result = subprocess.run(
        [os.environ['COMSPEC'], '/d', '/c', 'driver.cmd'],
        cwd=package, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        startupinfo=startup, creationflags=subprocess.CREATE_NEW_CONSOLE, timeout=40)
    (folder / 'output.bin').write_bytes(result.stdout)
    output = result.stdout.decode('gbk', errors='replace')
    assert b'65001' in result.stdout.splitlines()[-1], ('code page not restored', output)
    calls_file = state / 'calls.tsv'
    calls = [line.split('\t') for line in calls_file.read_text(encoding='utf-8').splitlines()] if calls_file.exists() else []
    expected = 1 if marker else 0
    assert result.returncode == expected, (name, result.returncode, output)
    if not marker:
        assert '[成功]' in output, (name, output)
    else:
        assert '[错误]' in output, (name, output)
    if check or marker in ('noadmin', 'badconfig', 'query-denied'):
        assert not any(c[0] == 'sc.exe' and c[1] in ('create', 'config', 'delete', 'stop') for c in calls), calls
    elif marker == 'fail-start':
        if existing:
            assert (state / 'hikbridge.service').read_text(encoding='utf-8') == original, calls
            assert (state / 'hikbridge.state').read_text() == '4', calls
            assert not any(c[:3] == ['sc.exe', 'delete', 'hikbridge'] for c in calls), calls
        else:
            assert not (state / 'hikbridge.service').exists(), calls
        if task:
            assert (state / 'HikSdkHttpBridge.task').exists(), calls
    elif marker == 'fail-recovery':
        assert (state / 'hikbridge.state').read_text() == '4', calls
        assert (state / 'HikSdkHttpBridge.task').exists(), calls
    elif marker == 'pending-delete':
        assert (state / 'hikbridge.service').exists(), calls
        assert '待删除' in output, output
    elif action == 'install':
        image = (state / 'hikbridge.service').read_text(encoding='utf-8')
        expected_image = f'"{package.resolve()}\\hik-sdk-http-bridge.exe" service --config "{package.resolve()}\\config.json"'
        assert image == expected_image, (image, expected_image, calls)
        assert (state / 'hikbridge.state').read_text() == '4', calls
        if existing:
            assert not any(c[:3] == ['sc.exe', 'delete', 'hikbridge'] for c in calls), calls
        if task:
            assert not (state / 'HikSdkHttpBridge.task').exists(), calls
            ready = next(i for i,c in enumerate(calls) if c[:3] == ['sc.exe', 'start', 'hikbridge'])
            removed = next(i for i,c in enumerate(calls) if c[:2] == ['schtasks.exe', '/Delete'])
            assert ready < removed, calls
    else:
        assert not (state / 'hikbridge.service').exists(), calls
        assert not (state / 'HikSdkHttpBridge.task').exists(), calls
        assert (package / 'config.json').exists()
    print('PASS', name)

scenario('check', check=True)
scenario('no-admin', marker='noadmin')
scenario('invalid-config', marker='badconfig')
scenario('query-error', marker='query-denied')
scenario('new-install', task=True)
scenario('repeat-install', existing=True)
scenario('rollback-new', task=True, marker='fail-start')
scenario('rollback-existing', existing=True, task=True, marker='fail-start')
scenario('recovery-config-error', task=True, marker='fail-recovery')
scenario('uninstall', existing=True, task=True, action='uninstall')
scenario('uninstall-absent', action='uninstall')
scenario('uninstall-pending-delete', existing=True, marker='pending-delete', action='uninstall')
print('PASS CMD flow, Chinese messages, paths with spaces/&/%/!, and native argument quoting')
