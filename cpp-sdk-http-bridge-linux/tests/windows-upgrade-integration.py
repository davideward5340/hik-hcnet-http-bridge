"""Elevated SCM tests using ONLY dedicated *-upgrade-test-* service names.
Never stop, reconfigure or delete the user's hikbridge/HikSdkHttpBridge services.
"""
from pathlib import Path
import argparse,subprocess,json,socket,time,os,shutil,ctypes
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
assert ctypes.windll.shell32.IsUserAnAdmin(),'Run elevated'
build=a.build.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
native='hikbridge-upgrade-test-native';managed='HikSdkHttpBridge-upgrade-test-managed';names=[native,managed]
flags=subprocess.CREATE_NO_WINDOW
def cmd(args,check=True):
 r=subprocess.run([str(x) for x in args],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,creationflags=flags,timeout=150)
 if check and r.returncode:raise RuntimeError(f'Command failed ({r.returncode}): {r.stdout.decode("gbk",errors="replace")}')
 return r
def sc(*args,check=True):return cmd([str(Path(os.environ['SystemRoot'])/'System32/sc.exe'),*args],check)
for name in names:
 assert sc('query',name,check=False).returncode==1060,f'Existing test registration {name}; refuse to disturb it'
 assert not (Path(os.environ['SystemRoot'])/'System32/Tasks'/name).exists(),'Existing test task'
baseline=cmd(['sc.exe','qc','hikbridge'],False).stdout
source_cs=out/'managed-fixture.cs'
source_cs.write_text('using System.ServiceProcess; class Fixture:ServiceBase { Fixture(string name){ServiceName=name;} static void Main(string[] args){ServiceBase.Run(new Fixture(args[1]));} }',encoding='utf-8')
cmd([str(Path(os.environ['SystemRoot'])/'Microsoft.NET/Framework/v4.0.30319/csc.exe'),'/nologo','/target:exe','/reference:System.ServiceProcess.dll','/out:'+str(out/'managed-fixture.exe'),source_cs])
def free_port():
 with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def wait_state(name,state):
 deadline=time.monotonic()+40
 while time.monotonic()<deadline:
  r=sc('query',name,check=False)
  if r.returncode==0 and state.encode() in r.stdout:return
  time.sleep(.2)
 raise RuntimeError('Service did not reach '+state)
def cleanup():
 for name in names:
  r=sc('query',name,check=False)
  if r.returncode==1060:continue
  sc('stop',name,check=False)
  try:wait_state(name,'STOPPED')
  except Exception:pass
  sc('delete',name,check=False)
  deadline=time.monotonic()+35
  while time.monotonic()<deadline:
   if sc('query',name,check=False).returncode==1060:break
   time.sleep(.2)
def scenario(label,is_managed=False,old_name=native,fail=False):
 folder=out/label;folder.mkdir();old=folder/'old 中文 空格 & %literal% !';new=folder/'new 中文 空格 & %literal% !';old.mkdir();new.mkdir()
 for dest in [old,new]:
  for part in ['hcnetsdk','ffmpeg']:(dest/part).mkdir()
  config=json.loads((build/'config.json').read_text())
  config['server']['port']=free_port();config['sdk']['directory']=str(build/'hcnetsdk');config['ffmpeg']['path']=str(build/'ffmpeg/ffmpeg.exe');config['ffmpeg']['hardwareAcceleration']='off';config['logging']['directory']=str(dest/'logs');config['media']['codecCacheFile']=str(dest/'state/cache.tsv')
  # The new config will be moved into the old directory; all non-runtime paths are relative.
  config['logging']['directory']='./logs';config['media']['codecCacheFile']='./state/cache.tsv'
  (dest/'config.json').write_text(json.dumps(config),encoding='utf-8')
  shutil.copy2(build/'hik-upgrade-service-fixture.exe',dest/'hik-sdk-http-bridge.exe')
 new_config=json.loads((new/'config.json').read_text());listener=None
 if fail:
  listener=socket.socket();listener.bind(('127.0.0.1',new_config['server']['port']));listener.listen()
 if is_managed:shutil.copy2(out/'managed-fixture.exe',old/'hik-sdk-http-bridge.exe')
 (old/'old-only.txt').write_text('preserve original in backup')
 (new/'nested').mkdir();(new/'nested/all-content.txt').write_text('copy every file')
 shutil.copy2(build/'version.json',new/'version.json')
 shutil.copy2(build/'hik-upgrade-integration-tool.exe',new/'hik-bridge-upgrade.exe')
 script=(build/'windows-service.cmd').read_bytes().decode('gbk').replace('HikSdkHttpBridge',managed).replace('hikbridge',native)
 (new/'windows-service.cmd').write_bytes(script.encode('gbk'))
 command='"'+str(old/'hik-sdk-http-bridge.exe')+'" '+('service-run '+old_name if is_managed else 'service --config "'+str(old/'config.json')+'"')
 try:
  sc('create',old_name,'binPath=',command,'start=','auto','obj=',r'NT AUTHORITY\LocalService' if is_managed else 'LocalSystem')
  sc('description',old_name,'original test service')
  sc('config',old_name,'start=','delayed-auto')
  sc('failure',old_name,'reset=','86400','actions=','restart/3000')
  sc('start',old_name);wait_state(old_name,'RUNNING')
  if label=='native-full-replace':
   ready=folder/'lock.ready'
   holder=subprocess.Popen([str(build/'hik-upgrade-unit-tests.exe'),'--hold-lock','Global\\HikBridge.Upgrade.'+native,str(ready)],creationflags=flags)
   try:
    deadline=time.monotonic()+5
    while not ready.exists() and time.monotonic()<deadline:time.sleep(.02)
    assert ready.exists(),'Mutex holder did not start'
    blocked=cmd([new/'hik-bridge-upgrade.exe','/check'],False)
    assert blocked.returncode!=0 and '已有升级程序正在运行' in blocked.stdout.decode('utf-8',errors='replace')
    assert (old/'old-only.txt').exists(),'Blocked upgrader changed the old directory'
   finally:
    Path(str(ready)+'.release').write_text('release');holder.wait(timeout=10)
   print('PASS competing upgrade process rejected before touching old installation',flush=True)
  check=cmd([new/'hik-bridge-upgrade.exe','/check'],False);(folder/'check.log').write_bytes(check.stdout);assert check.returncode==0,check.stdout.decode('utf-8',errors='replace')
  result=cmd([new/'hik-bridge-upgrade.exe'],False);(folder/'upgrade.log').write_bytes(result.stdout)
  if fail:
   assert result.returncode!=0,'Broken new port should fail'
   assert (old/'old-only.txt').exists(),'Rollback lost old directory'
   wait_state(old_name,'RUNNING')
   assert b'original test service' in sc('qdescription',old_name).stdout
   assert b'3000' in sc('qfailure',old_name).stdout,'Failure policy was not restored'
   key=chr(92).join(['HKLM','SYSTEM','CurrentControlSet','Services',old_name])
   assert b'0x1' in cmd(['reg.exe','query',key,'/v','DelayedAutoStart']).stdout,'Delayed startup policy was not restored'
   if is_managed:assert b'LocalService' in sc('qc',old_name).stdout
   assert not (old/'nested/all-content.txt').exists(),'New files leaked into restored installation'
  else:
   assert result.returncode==0,result.stdout.decode('utf-8',errors='replace')
   wait_state(native,'RUNNING');assert (old/'nested/all-content.txt').exists() and not (old/'old-only.txt').exists()
   assert list(folder.glob('old *-backup/old-only.txt')),'Full backup missing'
   if old_name!=native:assert sc('query',old_name,check=False).returncode==1060
  print('PASS',label,flush=True)
 finally:
  if listener:listener.close()
  cleanup()
try:
 scenario('native-full-replace')
 scenario('managed-other-name',True,managed)
 scenario('managed-same-name',True,native)
 scenario('native-rollback',fail=True)
 scenario('managed-rollback',True,managed,True)
 assert cmd(['sc.exe','qc','hikbridge'],False).stdout==baseline,'Production service configuration changed'
 print('PASS isolated upgrade/migration/rollback tests; production service configuration unchanged',flush=True)
finally:cleanup()
