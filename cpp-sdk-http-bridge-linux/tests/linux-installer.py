"""Check install.sh without checksum files using sandboxed paths and command doubles.
Never invoke real systemctl or write into /usr/local or /etc.
"""
from pathlib import Path
import os,subprocess,tempfile,shlex
source=Path(__file__).resolve().parents[1]/'deploy/linux/install.sh'
text=source.read_text();assert 'sha256' not in text
subprocess.run(['sh','-n',str(source)],check=True)
with tempfile.TemporaryDirectory(prefix='hik-installer-test-') as temp:
 root=Path(temp);mock=root/'tools';mock.mkdir();unit=root/'unit';unit.mkdir();state=root/'state';package=root/'package';package.mkdir()
 for name,body in {
  'id':'echo 0',
  'uname':'echo x86_64',
  'systemctl':'printf "%s\\n" "$*" >> "$HIK_TEST_LOG"; exit 0',
  'sha256sum':'echo checksum-was-called >> "$HIK_TEST_LOG"; exit 99',
 }.items():
  p=mock/name;p.write_text('#!/bin/sh\n'+body+'\n');p.chmod(0o755)
 rewritten=text.replace('INSTALL_DIR=/usr/local/hikbridge','INSTALL_DIR='+shlex.quote(str(root/'installed')))
 rewritten=rewritten.replace('STATE_DIR=/var/lib/hikbridge','STATE_DIR='+shlex.quote(str(state)))
 rewritten=rewritten.replace('UNIT_FILE=/etc/systemd/system/hikbridge.service','UNIT_FILE='+shlex.quote(str(unit/'hikbridge.service')))
 rewritten=rewritten.replace('[ -d /run/systemd/system ]','[ -d '+shlex.quote(str(unit))+' ]')
 install=package/'install.sh';install.write_text(rewritten)
 image_name=next(line.strip().split('=',1)[1] for line in text.splitlines() if line.strip().startswith('IMAGE_NAME=') and 'x86_64' in line)
 (package/image_name).write_bytes(b'test payload')
 (package/'uninstall.sh').write_text('#!/bin/sh\nexit 0\n')
 env=os.environ.copy();env['PATH']=str(mock)+':'+env['PATH'];env['HIK_TEST_LOG']=str(root/'commands.log');env['TMPDIR']=str(root)
 result=subprocess.run(['sh',str(install)],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=10)
 assert result.returncode==0,result.stdout.decode()
 assert (root/'installed/hikbridge.AppImage').read_bytes()==b'test payload'
 assert 'checksum-was-called' not in (root/'commands.log').read_text()
 print('PASS Linux install flow with no .sha256 file and unavailable checksum command (isolated filesystem/systemctl)')
