"""Assemble verified build outputs into the documented Windows/Linux full packages.
Run from any directory. See deploy/RELEASE.md for prerequisites and verification.
"""
from pathlib import Path
import argparse,datetime,hashlib,json,os,re,shutil,struct,subprocess,tarfile,tempfile,zipfile

PROJECT=Path(__file__).resolve().parents[1]
def require(value,message):
    if not value:raise RuntimeError(message)
def digest(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def identity():
    text=(PROJECT/'src/version.h').read_text(encoding='utf-8')
    def field(name):return re.search(r'^#define '+name+r' "([^"]+)"',text,re.M).group(1)
    return {'version':field('HIK_BRIDGE_VERSION'),'releaseVersion':field('HIK_BRIDGE_RELEASE_VERSION'),'buildDate':field('HIK_BRIDGE_BUILD_DATE'),'capabilities':{'cleanSessions':True}}
def rendered(text,info):
    return text.replace('@VERSION@',info['version']).replace('@BUILD_DATE@',info['buildDate'])
def pe_machine(path):
    data=path.read_bytes();require(data[:2]==b'MZ',f'Invalid PE: {path}')
    return struct.unpack_from('<H',data,struct.unpack_from('<I',data,60)[0]+4)[0]
def runtime_copy(source,target):
    require(source.is_dir(),f'Missing runtime: {source}')
    for path in [source,*source.rglob('*')]:require(not path.is_symlink(),f'Runtime symlink unsupported: {path}')
    shutil.copytree(source,target,ignore=shutil.ignore_patterns('logs','*.log','*.pdb','installation-backups'))
def common(package,platform,info):
    (package/'version.json').write_bytes((json.dumps(info,ensure_ascii=False,indent=2)+'\n').encode('utf-8'))
    usage=rendered((PROJECT/'deploy'/platform/'USAGE.txt').read_text(encoding='utf-8-sig'),info)
    (package/'使用说明.txt').write_text(usage,encoding='utf-8-sig',newline='\r\n' if platform=='windows' else '\n')
    (package/'本次更新说明.txt').write_text(rendered((PROJECT/'deploy/RELEASE_NOTES.txt').read_text(encoding='utf-8'),info),encoding='utf-8-sig')
def windows_package(stage,build,info):
    require(build.is_dir(),f'Windows build directory missing: {build}')
    require(json.loads((build/'version.json').read_text())['version']==info['version'],'Windows build manifest version mismatch; rebuild first')
    if os.name=='nt':
        result=subprocess.check_output([str(build/'hik-sdk-http-bridge.exe'),'--version'],creationflags=subprocess.CREATE_NO_WINDOW,timeout=15).decode().strip()
        require(result=='hik-sdk-http-bridge '+info['version'],'Windows binary version mismatch')
    package=stage/'windows/hikbridge';package.mkdir(parents=True)
    for folder in ['hcnetsdk','ffmpeg']:runtime_copy(build/folder,package/folder)
    for name in ['hik-sdk-http-bridge.exe','hik-bridge-upgrade.exe']:shutil.copy2(build/name,package/name)
    for name in ['安装.bat','卸载.bat','升级.bat','run-bridge.cmd','windows-service.cmd']:
        source=PROJECT/'deploy/windows'/name;data=source.read_bytes();data.decode('gbk')
        require(b'\xef\xbb\xbf'!=data[:3] and b'\n' not in data.replace(b'\r\n',b''),f'Expected GBK/CRLF: {source}')
        (package/name).write_bytes(data)
    shutil.copy2(PROJECT/'config/config.windows.json',package/'config.json')
    for name in ['LICENSE','NOTICE']:shutil.copy2(PROJECT.parent/name,package/name)
    for name in ['logs','state']:(package/name).mkdir()
    (package/'升级说明.txt').write_text((PROJECT/'UPGRADE_WINDOWS.md').read_text(encoding='utf-8'),encoding='utf-8-sig')
    common(package,'windows',info)
    for name in ['hik-sdk-http-bridge.exe','hik-bridge-upgrade.exe','hcnetsdk/HCNetSDK.dll']:
        require(pe_machine(package/name)==0x14c,f'Expected x86: {name}')
    require(pe_machine(package/'ffmpeg/ffmpeg.exe')==0x8664,'Current release rule expects x64 FFmpeg')
    for dll in (PROJECT/'ffmpeglibs').glob('*.dll'):
        require(digest(dll)==digest(package/'ffmpeg'/dll.name),f'FFmpeg DLL mismatch: {dll.name}')
    output=stage/'hikbridge.zip'
    with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
        archive.write(package,'hikbridge/')
        for path in sorted(package.rglob('*')):archive.write(path,'hikbridge/'+path.relative_to(package).as_posix())
    with zipfile.ZipFile(output) as archive:
        require(archive.testzip() is None,'ZIP integrity failure')
        require(not any(n.endswith(('.log','.pdb')) or 'fixture' in n for n in archive.namelist()),'Development output in ZIP')
    return output
def linux_package(stage,dist,info):
    package=stage/'linux/hikbridge';package.mkdir(parents=True)
    for name in ['install.sh','uninstall.sh','start.sh']:
        source=(PROJECT/'deploy/linux'/name).read_bytes()
        require(b'\r' not in source,'Linux source must use LF')
        text=source.decode('utf-8')
        text=re.sub(r'hik-sdk-http-bridge-[0-9.]+-',lambda _: 'hik-sdk-http-bridge-'+info['version']+'-',text)
        require('\r' not in text,'Linux source must use LF')
        (package/name).write_bytes(text.encode('utf-8'))
    require('sha256' not in (package/'install.sh').read_text(encoding='utf-8'),'Linux installer must not perform checksum validation')
    for arch,machine in [('x86_64',62),('aarch64',183)]:
        name=f'hik-sdk-http-bridge-{info["version"]}-{arch}-glibc2.23.AppImage';source=dist/name
        with source.open('rb') as stream:header=stream.read(20)
        require(header[:4]==b'\x7fELF' and struct.unpack_from('<H',header,18)[0]==machine,f'Wrong AppImage architecture: {name}')
        expected=digest(source);require((dist/(name+'.sha256')).read_text().split()[0]==expected,f'AppImage checksum mismatch: {name}')
        shutil.copy2(source,package/name)
        (package/(name+'.sha256')).write_text(expected+'  '+name+'\n',encoding='ascii')
    common(package,'linux',info)
    def metadata(item):
        item.uid=item.gid=0;item.uname=item.gname='root';item.mode=0o755 if item.isdir() or item.name.endswith(('.sh','.AppImage')) else 0o644
        return item
    output=stage/'hikbridge.tar.gz'
    with tarfile.open(output,'w:gz',compresslevel=6) as archive:archive.add(package,arcname='hikbridge',filter=metadata)
    with tarfile.open(output,'r:gz') as archive:
        require(all(m.name=='hikbridge' or m.name.startswith('hikbridge/') for m in archive.getmembers()),'Unexpected TAR root')
        for name in ['install.sh','uninstall.sh','start.sh']:require(archive.getmember('hikbridge/'+name).mode&0o111,'Shell script missing executable mode')
    return output
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform',choices=['all','windows','linux'],default='all')
    parser.add_argument('--windows-build',type=Path)
    parser.add_argument('--linux-dist',type=Path,default=PROJECT/'dist')
    parser.add_argument('--output',type=Path,default=PROJECT/'dist/releases')
    args=parser.parse_args();info=identity();output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    stages=output/'.release-stage';stages.mkdir(exist_ok=True)
    stage=Path(tempfile.mkdtemp(prefix=info['version']+'-',dir=stages)).resolve()
    try:
        products=[]
        if args.platform in ['all','windows']:products.append(windows_package(stage,(args.windows_build or PROJECT/('build-win32-'+info['version'])).resolve(),info))
        if args.platform in ['all','linux']:products.append(linux_package(stage,args.linux_dist.resolve(),info))
        stamp=datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+str(os.getpid())
        for product in products:
            target=output/product.name
            if target.exists():
                backup=output/'.release-backups'/stamp;backup.mkdir(parents=True,exist_ok=True);shutil.copy2(target,backup/target.name)
                sidecar=output/(target.name+'.sha256')
                if sidecar.exists():shutil.copy2(sidecar,backup/sidecar.name)
            os.replace(product,target)
            value=digest(target);(output/(target.name+'.sha256')).write_text(value+'  '+target.name+'\n',encoding='ascii')
            print(target,info['version'],target.stat().st_size,'bytes',value)
    finally:
        # Recursive cleanup is limited to the unique temporary directory we just created.
        require(stage.parent==stages.resolve() and stages.resolve().parent==output,'Unsafe staging cleanup path')
        shutil.rmtree(stage)
if __name__=='__main__':main()
