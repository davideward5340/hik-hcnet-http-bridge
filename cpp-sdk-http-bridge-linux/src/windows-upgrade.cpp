// Native companion to 升级.bat. No PowerShell, WMIC or curl dependency.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <regex>
#include <stdexcept>
#include <memory>
#include <thread>
#include "version.h"

namespace fs = std::filesystem;
namespace upgrade {
#ifndef HIK_UPGRADE_NATIVE_SERVICE
#define HIK_UPGRADE_NATIVE_SERVICE L"hikbridge"
#define HIK_UPGRADE_MANAGED_SERVICE L"HikSdkHttpBridge"
#endif
constexpr const wchar_t* native_name=HIK_UPGRADE_NATIVE_SERVICE;
constexpr const wchar_t* managed_name=HIK_UPGRADE_MANAGED_SERVICE;
struct Handle { HANDLE h{}; ~Handle(){ if(h && h!=INVALID_HANDLE_VALUE) CloseHandle(h); } };
struct ServiceHandle { SC_HANDLE h{}; ~ServiceHandle(){ if(h) CloseServiceHandle(h); } };
void fail(const std::string& text) { throw std::runtime_error(text + " (Windows error " + std::to_string(GetLastError()) + ")"); }
void require(bool test, const std::string& message) { if(!test) throw std::runtime_error(message); }
class UpgradeLock {
    HANDLE mutex_{};
public:
    explicit UpgradeLock(const std::wstring& name = std::wstring(L"Global\\HikBridge.Upgrade.") + native_name) {
        mutex_=CreateMutexW(nullptr,FALSE,name.c_str());
        if(!mutex_)fail("无法创建升级互斥锁");
        const DWORD result=WaitForSingleObject(mutex_,0);
        if(result!=WAIT_OBJECT_0) {
            if(result==WAIT_ABANDONED)ReleaseMutex(mutex_);
            CloseHandle(mutex_);mutex_=nullptr;
            if(result==WAIT_ABANDONED)throw std::runtime_error("上次升级异常中止，请先检查备份和服务状态后重试");
            throw std::runtime_error("已有升级程序正在运行，请等待其完成后再试");
        }
    }
    UpgradeLock(const UpgradeLock&)=delete;
    UpgradeLock& operator=(const UpgradeLock&)=delete;
    ~UpgradeLock(){if(mutex_){ReleaseMutex(mutex_);CloseHandle(mutex_);}}
};
std::wstring wide(const std::string& text) {
    int n=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    std::wstring result(n,L'\0'); MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),n); return result;
}
void say(const std::wstring& text) {
    auto line=text+L"\r\n"; DWORD n,mode;
    if(GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE),&mode)) WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE),line.data(),static_cast<DWORD>(line.size()),&n,nullptr);
    else { int size=WideCharToMultiByte(CP_UTF8,0,line.data(),static_cast<int>(line.size()),nullptr,0,nullptr,nullptr); std::string s(size,'\0'); WideCharToMultiByte(CP_UTF8,0,line.data(),static_cast<int>(line.size()),s.data(),size,nullptr,nullptr); WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data(),size,&n,nullptr); }
}
std::wstring lower(std::wstring value) { std::transform(value.begin(),value.end(),value.begin(),::towlower); return value; }
bool same(const fs::path& a,const fs::path& b) { return lower(a.lexically_normal().wstring())==lower(b.lexically_normal().wstring()); }
bool contains(const fs::path& parent,const fs::path& child) {
    auto a=lower(parent.lexically_normal().wstring()),b=lower(child.lexically_normal().wstring());
    if(a.back()!=L'\\') a+=L'\\'; return b.size()>=a.size() && b.compare(0,a.size(),a)==0;
}
fs::path resolved_path(const fs::path& p) { return fs::canonical(p); }
void no_links(const fs::path& path) {
    fs::path part;
    for(const auto& segment:fs::absolute(path)) {
        part/=segment; auto attr=GetFileAttributesW(part.c_str());
        if(attr!=INVALID_FILE_ATTRIBUTES) require(!(attr&FILE_ATTRIBUTE_REPARSE_POINT),"Reparse points are not supported in upgrade paths");
    }
}
void validate_paths(const fs::path& source,const fs::path& target) {
    no_links(source); no_links(target);
    require(fs::is_directory(source)&&fs::is_directory(target),"Source/installation directory missing");
    require(!same(source,target)&&!contains(source,target)&&!contains(target,source),"新包目录不能与旧安装目录相同或互相包含，请解压到另一个独立目录");
    require(target!=target.root_path(),"Refusing to replace a drive root");
    wchar_t windows[MAX_PATH]; GetWindowsDirectoryW(windows,MAX_PATH);
    require(!same(target,windows)&&!contains(windows,target),"Refusing to replace a Windows system directory");
    for(const wchar_t* name:{L"ProgramFiles",L"ProgramFiles(x86)",L"USERPROFILE",L"ProgramData"}) {
        wchar_t value[32768]; if(GetEnvironmentVariableW(name,value,32768)) require(!same(target,value),"Refusing to replace a shared system/profile root");
    }
}
std::wstring quote(const std::wstring& value) {
    std::wstring result=L"\""; size_t slashes=0;
    for(wchar_t c:value) {
        if(c==L'\\'){ ++slashes; continue; }
        result.append(c==L'"'?slashes*2+1:slashes,L'\\'); slashes=0; result+=c;
    }
    result.append(slashes*2,L'\\'); return result+L"\"";
}
DWORD run(const fs::path& exe,const std::vector<std::wstring>& args,const fs::path& cwd,DWORD timeout=300000,const fs::path& output={}) {
    std::wstring line=quote(exe.wstring()); for(const auto& arg:args) line+=L" "+quote(arg);
    if(lower(exe.filename().wstring())==L"cmd.exe") {
        // CMD parses /c differently from a normal argv process. Keep its command fixed;
        // the potentially special-character installation path is only lpCurrentDirectory.
        require(args==std::vector<std::wstring>{L"/d",L"/c",L"windows-service.cmd install /nopause"},"Unexpected CMD command");
        line=quote(exe.wstring())+L" /d /c windows-service.cmd install /nopause";
    }
    STARTUPINFOW si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};Handle log,input;
    if(!output.empty()) {
        log.h=CreateFileW(output.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&attributes,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        input.h=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&attributes,OPEN_EXISTING,0,nullptr);
        if(log.h==INVALID_HANDLE_VALUE||input.h==INVALID_HANDLE_VALUE)fail("Cannot create upgrade command log");
        si.dwFlags|=STARTF_USESTDHANDLES;si.hStdOutput=si.hStdError=log.h;si.hStdInput=input.h;
    }
    if(!CreateProcessW(exe.c_str(),line.data(),nullptr,nullptr,!output.empty(),CREATE_NO_WINDOW,nullptr,cwd.c_str(),&si,&pi)) fail("Cannot start child process");
    Handle process{pi.hProcess},thread{pi.hThread};
    if(WaitForSingleObject(process.h,timeout)!=WAIT_OBJECT_0) { TerminateProcess(process.h,ERROR_TIMEOUT); WaitForSingleObject(process.h,10000); throw std::runtime_error("Child process timed out"); }
    DWORD code; if(!GetExitCodeProcess(process.h,&code)) fail("Cannot read process exit code"); return code;
}
std::vector<std::wstring> arguments(const std::wstring& value) {
    DWORD size=ExpandEnvironmentStringsW(value.c_str(),nullptr,0); std::wstring expanded(size,L'\0'); ExpandEnvironmentStringsW(value.c_str(),expanded.data(),size);
    int count=0; auto list=CommandLineToArgvW(expanded.c_str(),&count); if(!list) fail("Cannot parse service command");
    std::vector<std::wstring> result(list,list+count); LocalFree(list); return result;
}
std::string read(const fs::path& path) {
    std::ifstream file(path,std::ios::binary); require(static_cast<bool>(file),"Cannot read file"); return {std::istreambuf_iterator<char>(file),{}};
}
template<typename T> T at(const std::string& bytes,size_t offset) {
    require(offset<=bytes.size()&&sizeof(T)<=bytes.size()-offset,"Invalid PE executable"); T value; memcpy(&value,bytes.data()+offset,sizeof(value)); return value;
}
bool managed_pe(const fs::path& exe) {
    const auto bytes=read(exe); require(at<WORD>(bytes,0)==IMAGE_DOS_SIGNATURE,"Not a Windows executable");
    DWORD pe=at<DWORD>(bytes,0x3c); require(at<DWORD>(bytes,pe)==IMAGE_NT_SIGNATURE,"Invalid PE signature");
    const auto optional=pe+4+sizeof(IMAGE_FILE_HEADER); WORD magic=at<WORD>(bytes,optional);
    require(magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC||magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC,"Unsupported PE format");
    size_t directory=optional+(magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC?96:112)+IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR*sizeof(IMAGE_DATA_DIRECTORY);
    return at<IMAGE_DATA_DIRECTORY>(bytes,directory).VirtualAddress!=0;
}
enum class Kind { Native, Managed };
Kind identify(const fs::path& exe,const std::vector<std::wstring>& args) {
    auto name=lower(exe.filename().wstring());
    require(name==L"hik-sdk-http-bridge.exe"||name==L"hiksdkhttpbridge.exe","Unknown service executable; refusing migration");
    require(args.size()>=2,"Service executable has no recognized service-mode argument");
    bool managed=managed_pe(exe);
    if(managed) { require(lower(args[1])==L"service-run","Managed executable is not a supported C# service"); return Kind::Managed; }
    require(name==L"hik-sdk-http-bridge.exe"&&lower(args[1])==L"service","Executable is not the native C++ service");
    require(read(exe).find("hik-sdk-http-bridge")!=std::string::npos,"Missing native bridge product identity"); return Kind::Native;
}
struct Snapshot {
    std::wstring name,command,display,account,group,description; std::vector<wchar_t> dependencies;
    DWORD type{},start{},error{}; bool running{},delayed{},failureFlag{};
    std::vector<BYTE> failures; Kind kind{}; fs::path exe;
};
std::vector<BYTE> config2(SC_HANDLE svc,DWORD level) {
    DWORD n=0; QueryServiceConfig2W(svc,level,nullptr,0,&n); if(!n) return {};
    std::vector<BYTE> result(n); if(!QueryServiceConfig2W(svc,level,result.data(),n,&n)) fail("Cannot query extended service configuration"); return result;
}
SERVICE_STATUS_PROCESS status(SC_HANDLE svc) {
    SERVICE_STATUS_PROCESS state{}; DWORD n; if(!QueryServiceStatusEx(svc,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&state),sizeof(state),&n)) fail("Cannot query service status"); return state;
}
Snapshot snapshot(SC_HANDLE manager,const std::wstring& name) {
    ServiceHandle svc{OpenServiceW(manager,name.c_str(),SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS)}; if(!svc.h) fail("Cannot open service");
    DWORD n=0; QueryServiceConfigW(svc.h,nullptr,0,&n); std::vector<BYTE> data(n);
    if(!QueryServiceConfigW(svc.h,reinterpret_cast<QUERY_SERVICE_CONFIGW*>(data.data()),n,&n)) fail("Cannot query service configuration");
    auto c=reinterpret_cast<QUERY_SERVICE_CONFIGW*>(data.data()); Snapshot s;
    s.name=name;s.command=c->lpBinaryPathName;s.display=c->lpDisplayName;s.account=c->lpServiceStartName;s.group=c->lpLoadOrderGroup;s.type=c->dwServiceType;s.start=c->dwStartType;s.error=c->dwErrorControl;
    if(c->lpDependencies){auto p=c->lpDependencies;while(*p)p+=wcslen(p)+1;s.dependencies.assign(c->lpDependencies,p+1);} if(s.dependencies.empty())s.dependencies={0,0};
    auto state=status(svc.h); require(state.dwCurrentState==SERVICE_RUNNING||state.dwCurrentState==SERVICE_STOPPED,"服务正在启动或停止，请等待状态稳定后重试");s.running=state.dwCurrentState==SERVICE_RUNNING;
    auto delayed=config2(svc.h,SERVICE_CONFIG_DELAYED_AUTO_START_INFO);if(!delayed.empty())s.delayed=reinterpret_cast<SERVICE_DELAYED_AUTO_START_INFO*>(delayed.data())->fDelayedAutostart!=0;
    auto description=config2(svc.h,SERVICE_CONFIG_DESCRIPTION); if(!description.empty()){auto d=reinterpret_cast<SERVICE_DESCRIPTIONW*>(description.data());if(d->lpDescription)s.description=d->lpDescription;}
    s.failures=config2(svc.h,SERVICE_CONFIG_FAILURE_ACTIONS);
    auto flag=config2(svc.h,SERVICE_CONFIG_FAILURE_ACTIONS_FLAG);if(!flag.empty())s.failureFlag=reinterpret_cast<SERVICE_FAILURE_ACTIONS_FLAG*>(flag.data())->fFailureActionsOnNonCrashFailures!=0;
    auto args=arguments(s.command);require(!args.empty()&&fs::path(args[0]).is_absolute(),"Service executable path must be absolute and correctly quoted");
    no_links(args[0]);s.exe=resolved_path(args[0]);s.kind=identify(s.exe,args);
    require(s.type==SERVICE_WIN32_OWN_PROCESS,"Shared-process or driver services are unsupported");
    auto account=lower(s.account);require(account==L"localsystem"||account==L"nt authority\\localservice"||account==L"nt authority\\networkservice","旧服务使用自定义账户，自动回滚无法恢复其密码，请由管理员手动迁移");
    return s;
}
Snapshot discover(SC_HANDLE manager) {
    std::vector<std::wstring> found;
    for(const auto* name:{native_name,managed_name}) {
        ServiceHandle svc{OpenServiceW(manager,name,SERVICE_QUERY_CONFIG)};
        if(svc.h)found.emplace_back(name); else if(GetLastError()!=ERROR_SERVICE_DOES_NOT_EXIST)fail("Cannot inspect existing bridge service");
    }
    require(found.size()==1,found.empty()?"未找到支持的桥接服务，请使用安装.bat；仅使用计划任务的旧版需单独迁移":"同时发现多个桥接服务，请先处理重复注册，避免替换错误目录");
    return snapshot(manager,found[0]);
}
void stop(SC_HANDLE manager,const std::wstring& name) {
    ServiceHandle svc{OpenServiceW(manager,name.c_str(),SERVICE_STOP|SERVICE_QUERY_STATUS)};
    if(!svc.h){if(GetLastError()==ERROR_SERVICE_DOES_NOT_EXIST)return;fail("Cannot open service to stop");}
    auto state=status(svc.h);Handle process{state.dwProcessId?OpenProcess(SYNCHRONIZE,FALSE,state.dwProcessId):nullptr};
    if(state.dwCurrentState!=SERVICE_STOPPED&&state.dwCurrentState!=SERVICE_STOP_PENDING){SERVICE_STATUS unused;if(!ControlService(svc.h,SERVICE_CONTROL_STOP,&unused)&&GetLastError()!=ERROR_SERVICE_NOT_ACTIVE)fail("Cannot stop service");}
    ULONGLONG deadline=GetTickCount64()+90000;
    while(status(svc.h).dwCurrentState!=SERVICE_STOPPED){require(GetTickCount64()<deadline,"等待旧服务停止超时，升级已中止");Sleep(200);}
    if(process.h)require(WaitForSingleObject(process.h,15000)==WAIT_OBJECT_0,"Service process has not exited");
}
void remove(SC_HANDLE manager,const std::wstring& name) {
    stop(manager,name);
    {ServiceHandle svc{OpenServiceW(manager,name.c_str(),DELETE)};if(svc.h){if(!DeleteService(svc.h)&&GetLastError()!=ERROR_SERVICE_MARKED_FOR_DELETE)fail("Cannot delete old service");}else if(GetLastError()!=ERROR_SERVICE_DOES_NOT_EXIST)fail("Cannot open service for deletion");}
    ULONGLONG deadline=GetTickCount64()+30000;
    while(true){ServiceHandle svc{OpenServiceW(manager,name.c_str(),SERVICE_QUERY_STATUS)};if(!svc.h&&GetLastError()==ERROR_SERVICE_DOES_NOT_EXIST)return;require(GetTickCount64()<deadline,"旧服务仍处于待删除状态，请关闭服务管理窗口后重试");Sleep(200);}
}
void restore(SC_HANDLE manager,const Snapshot& s) {
    ServiceHandle svc{OpenServiceW(manager,s.name.c_str(),SERVICE_ALL_ACCESS)};
    if(!svc.h) {
        svc.h=CreateServiceW(manager,s.name.c_str(),s.display.c_str(),SERVICE_ALL_ACCESS,s.type,s.start,s.error,s.command.c_str(),s.group.empty()?nullptr:s.group.c_str(),nullptr,s.dependencies.data(),s.account.c_str(),nullptr);
        if(!svc.h)fail("Cannot restore old service registration");
    } else if(!ChangeServiceConfigW(svc.h,s.type,s.start,s.error,s.command.c_str(),s.group.c_str(),nullptr,s.dependencies.data(),s.account.c_str(),nullptr,s.display.c_str()))fail("Cannot restore old service settings");
    SERVICE_DELAYED_AUTO_START_INFO delay{s.delayed};if(!ChangeServiceConfig2W(svc.h,SERVICE_CONFIG_DELAYED_AUTO_START_INFO,&delay))fail("Cannot restore delayed startup setting");
    SERVICE_DESCRIPTIONW desc{const_cast<wchar_t*>(s.description.c_str())};if(!ChangeServiceConfig2W(svc.h,SERVICE_CONFIG_DESCRIPTION,&desc))fail("Cannot restore description");
    if(!s.failures.empty()&&!ChangeServiceConfig2W(svc.h,SERVICE_CONFIG_FAILURE_ACTIONS,const_cast<BYTE*>(s.failures.data())))fail("Cannot restore failure recovery");
    SERVICE_FAILURE_ACTIONS_FLAG flag{s.failureFlag};if(!ChangeServiceConfig2W(svc.h,SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,&flag))fail("Cannot restore recovery flag");
    if(s.running){if(!StartServiceW(svc.h,0,nullptr)&&GetLastError()!=ERROR_SERVICE_ALREADY_RUNNING)fail("Cannot restart original service");auto deadline=GetTickCount64()+60000;while(status(svc.h).dwCurrentState!=SERVICE_RUNNING){require(GetTickCount64()<deadline,"Original service did not resume running");Sleep(250);}}
}
bool equal_file(const fs::path& a,const fs::path& b) {
    if(fs::file_size(a)!=fs::file_size(b))return false;
    std::ifstream x(a,std::ios::binary),y(b,std::ios::binary);char xb[65536],yb[65536];
    while(x){x.read(xb,sizeof(xb));auto n=x.gcount();y.read(yb,n);if(y.gcount()!=n||memcmp(xb,yb,static_cast<size_t>(n)))return false;}return !x.bad()&&!y.bad();
}
void copy_all(const fs::path& from,const fs::path& to) {
    require(!fs::exists(to),"Staging directory already exists"); fs::create_directory(to);
    for(const auto& item:fs::recursive_directory_iterator(from)) {
        no_links(item.path());auto destination=to/fs::relative(item.path(),from);
        if(item.is_directory())fs::create_directory(destination);
        else {require(item.is_regular_file(),"Unsupported entry in full package");fs::copy_file(item.path(),destination);require(equal_file(item.path(),destination),"Copied file verification failed");}
    }
}
std::string json_string(const std::string& json,const std::string& key) {
    std::smatch match;std::regex expression("\""+key+"\"\\s*:\\s*\"([^\"]*)\"");require(std::regex_search(json,match,expression),"Missing JSON string field");return match[1];
}
int port(const fs::path& file) {
    auto text=read(file);std::smatch server,number;require(std::regex_search(text,server,std::regex("\"server\"\\s*:\\s*\\{([^}]*)\\}")),"Missing server configuration");
    auto fields=server[1].str();require(json_string(fields,"bind")=="127.0.0.1","Only the loopback bridge is supported");
    require(std::regex_search(fields,number,std::regex("\"port\"\\s*:\\s*([0-9]+)")),"Missing server port");int value=std::stoi(number[1]);require(value>0&&value<65536,"Invalid port");return value;
}
std::string get(int port,const wchar_t* path) {
    struct Internet {HINTERNET h;~Internet(){if(h)WinHttpCloseHandle(h);}};
    Internet session{WinHttpOpen(L"HikBridgeUpgrade",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};if(!session.h)fail("Cannot initialize HTTP verification");
    WinHttpSetTimeouts(session.h,1000,1000,1000,1000);
    Internet connection{WinHttpConnect(session.h,L"127.0.0.1",static_cast<INTERNET_PORT>(port),0)};if(!connection.h)fail("Cannot connect to HTTP service");
    Internet request{WinHttpOpenRequest(connection.h,L"GET",path,nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0)};if(!request.h)fail("Cannot create HTTP request");
    if(!WinHttpSendRequest(request.h,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request.h,nullptr))fail("HTTP verification failed");
    DWORD code=0,n=sizeof(code);WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&code,&n,nullptr);require(code==200,"Bridge HTTP returned non-200 status");
    std::string response;char buffer[4096];while(true){DWORD received=0;if(!WinHttpReadData(request.h,buffer,sizeof(buffer),&received))fail("Cannot read HTTP response");if(!received)break;response.append(buffer,received);require(response.size()<65536,"Unexpectedly large HTTP response");}return response;
}
bool owns_port(DWORD pid,int port) {
    DWORD size=0;GetExtendedTcpTable(nullptr,&size,FALSE,AF_INET,TCP_TABLE_OWNER_PID_LISTENER,0);std::vector<BYTE> bytes(size);
    if(GetExtendedTcpTable(bytes.data(),&size,FALSE,AF_INET,TCP_TABLE_OWNER_PID_LISTENER,0)!=NO_ERROR)return false;
    auto table=reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(bytes.data());
    for(DWORD i=0;i<table->dwNumEntries;i++)if(table->table[i].dwOwningPid==pid&&ntohs(static_cast<u_short>(table->table[i].dwLocalPort))==port)return true;
    return false;
}
void verify(SC_HANDLE manager,const fs::path& target,int listen_port,const std::string& expected) {
    auto deadline=GetTickCount64()+45000;std::string last;
    do { try {
        auto current=snapshot(manager,native_name);require(current.kind==Kind::Native&&current.running&&current.start==SERVICE_AUTO_START&&!current.delayed,"New service startup mode or state is incorrect");
        require(same(current.exe,target/L"hik-sdk-http-bridge.exe"),"New service points to wrong directory");
        ServiceHandle svc{OpenServiceW(manager,native_name,SERVICE_QUERY_STATUS)};require(svc.h!=nullptr,"New service missing");auto pid=status(svc.h).dwProcessId;
        require(owns_port(pid,listen_port),"Health port is not owned by the new service");
        auto health=get(listen_port,L"/healthz");require(json_string(health,"status")=="ok"&&json_string(health,"sdk")=="hcnetsdk","Service is unhealthy");
        require(json_string(get(listen_port,L"/version"),"version")==expected,"Running service version differs from new package");return;
    }catch(const std::exception& e){last=e.what();Sleep(500);} }while(GetTickCount64()<deadline);
    throw std::runtime_error("Upgrade verification failed: "+last);
}
fs::path self_directory(){std::vector<wchar_t> path(32768);DWORD n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));require(n&&n<path.size(),"Cannot locate updater");return resolved_path(fs::path(std::wstring(path.data(),n)).parent_path());}
fs::path system_tool(const wchar_t* name){wchar_t path[MAX_PATH];GetSystemDirectoryW(path,MAX_PATH);return fs::path(path)/name;}
bool admin(){SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;PSID sid=nullptr;BOOL member=FALSE;if(AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&sid)){CheckTokenMembership(nullptr,sid,&member);FreeSid(sid);}return member!=FALSE;}
int execute(bool check) {
    auto source=self_directory();require(admin(),"请右键升级.bat，选择以管理员身份运行");
    UpgradeLock transaction_lock; // Held across discovery, replacement, verification and rollback.
    require(fs::is_regular_file(source/L"windows-service.cmd")&&fs::is_regular_file(source/L"config.json")&&fs::is_directory(source/L"hcnetsdk")&&fs::is_directory(source/L"ffmpeg"),"新包不完整，请保留主程序、配置、SDK、FFmpeg 和服务安装脚本");
    auto newexe=source/L"hik-sdk-http-bridge.exe";require(!managed_pe(newexe),"Upgrade source must be the native C++ bridge");
    require(run(newexe,{L"--validate-config",L"--config",(source/L"config.json").wstring()},source,30000)==0,"新程序或配置校验失败，原服务尚未修改");
    auto expected=json_string(read(source/L"version.json"),"version");require(expected==HIK_BRIDGE_VERSION,"升级工具与新包版本不一致，请重新解压完整升级包");int listen_port=port(source/L"config.json");
    ServiceHandle manager{OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT|SC_MANAGER_CREATE_SERVICE)};if(!manager.h)fail("Cannot open service manager");
    auto old=discover(manager.h);auto target=resolved_path(old.exe.parent_path());validate_paths(source,target);
    // The installation script can remove legacy tasks. Do not silently mutate registrations
    // that this service-only transaction cannot restore itself.
    wchar_t windows[MAX_PATH];GetWindowsDirectoryW(windows,MAX_PATH);
    BOOL wow64=FALSE;IsWow64Process(GetCurrentProcess(),&wow64);
    for(const auto* name:{native_name,managed_name}) {
        const auto tasks=fs::path(windows)/(wow64?L"Sysnative":L"System32")/L"Tasks";
        require(!fs::exists(tasks/name)&&run(system_tool(L"schtasks.exe"),{L"/Query",L"/TN",name},source,10000)!=0,
            "发现残留的旧计划任务，请先处理计划任务迁移后再升级服务");
    }
    say(L"[信息] 原服务："+old.name+(old.kind==Kind::Native?L"（C++ 原生服务）":L"（C# 服务）"));say(L"[信息] 安装目录："+target.wstring());say(L"[信息] 将全量替换目录内容和配置；新版本："+wide(expected));
    for(const auto& item:fs::recursive_directory_iterator(source))no_links(item.path());
    // Reject directory aliases before any recursive operation or rename.
    for(const auto& item:fs::recursive_directory_iterator(target))no_links(item.path());
    if(check){say(L"[成功] 升级预检查通过，未修改文件或服务。");return 0;}
    auto suffix=L".upgrade-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(GetCurrentProcessId());
    auto backup=target.parent_path()/(target.filename().wstring()+suffix+L"-backup");auto staged=target.parent_path()/(target.filename().wstring()+suffix+L"-new");auto failed=target.parent_path()/(target.filename().wstring()+suffix+L"-failed");
    require(!fs::exists(backup)&&!fs::exists(staged)&&!fs::exists(failed),"Upgrade staging paths already exist");
    copy_all(source,staged); // All source files, including directories, copied and compared before downtime.
    require(run(staged/L"hik-sdk-http-bridge.exe",{L"--validate-config",L"--config",(staged/L"config.json").wstring()},staged,30000)==0,"Staged configuration/runtime validation failed");
    auto registry_backup=target.parent_path()/(target.filename().wstring()+suffix+L".reg");
    require(run(system_tool(L"reg.exe"),{L"export",L"HKLM\\SYSTEM\\CurrentControlSet\\Services\\"+old.name,registry_backup.wstring(),L"/y"},source,30000)==0,"Cannot back up service registration");
    say(L"[信息] 完整目录备份："+backup.wstring());say(L"[信息] 注册备份："+registry_backup.wstring());
    bool moved=false,installed=false;
    try {
        stop(manager.h,old.name);
        fs::rename(target,backup);moved=true;fs::rename(staged,target);
        require(run(target/L"hik-sdk-http-bridge.exe",{L"--validate-config",L"--config",(target/L"config.json").wstring()},target,30000)==0,"Installed package validation failed");
        if(old.kind==Kind::Managed)remove(manager.h,old.name);
        installed=true;
        require(run(system_tool(L"cmd.exe"),{L"/d",L"/c",L"windows-service.cmd install /nopause"},target,300000,target/L"upgrade-install.log")==0,"服务安装脚本执行失败，请查看新目录或失败目录内的 upgrade-install.log");
        verify(manager.h,target,listen_port,expected);
        say(L"[成功] 全量升级完成，服务为普通自动启动，健康检查和版本校验通过。");say(L"[信息] 旧目录备份保留在："+backup.wstring());return 0;
    }catch(const std::exception& error){
        say(L"[错误] "+wide(error.what()));say(L"[信息] 正在尝试恢复原文件和服务…");
        try {
            if(installed){stop(manager.h,native_name);if(old.name!=native_name)remove(manager.h,native_name);}
            else stop(manager.h,old.name);
            if(moved){if(fs::exists(target))fs::rename(target,failed);fs::rename(backup,target);}
            restore(manager.h,old);
            say(L"[信息] 已恢复原目录、服务配置及原运行状态。失败的新目录保留供排查。");
        }catch(const std::exception& rollback){say(L"[错误] 自动回滚未完成："+wide(rollback.what()));say(L"[错误] 请保留备份并人工恢复："+backup.wstring()+L"；"+registry_backup.wstring());}
        return 1;
    }
}
} // namespace upgrade

#ifndef HIK_UPGRADE_TEST
int wmain(int argc,wchar_t** argv) {
    try {
        bool check=false;
        if(argc==2&&std::wstring(argv[1])==L"/check")check=true;
        else upgrade::require(argc==1,"仅支持不带参数执行升级，或使用 /check 执行只读预检查");
        return upgrade::execute(check);
    }catch(const std::exception& e){upgrade::say(L"[错误] "+upgrade::wide(e.what()));return 1;}
}
#endif
