#define HIK_UPGRADE_TEST
#include "../src/windows-upgrade.cpp"

int wmain(int argc,wchar_t** argv) {
    using namespace upgrade;
    try {
        if(argc==4&&std::wstring(argv[1])==L"--hold-lock") {
            UpgradeLock lock(argv[2]);fs::path ready=argv[3];std::ofstream(ready)<<"locked";
            auto deadline=GetTickCount64()+10000;
            while(!fs::exists(ready.wstring()+L".release")){require(GetTickCount64()<deadline,"Lock holder timeout");Sleep(10);}
            return 0;
        }
        require(argc==3,"Supply a scratch directory and bridge executable");
        auto base=fs::absolute(argv[1])/(L"case-"+std::to_wstring(GetTickCount64()));fs::create_directories(base);
        {
            auto name=L"Local\\HikBridge.Upgrade.Unit."+std::to_wstring(GetCurrentProcessId());auto ready=base/L"mutex.ready";
            DWORD child_result=1;
            std::thread child([&]{try{child_result=run(fs::absolute(argv[0]),{L"--hold-lock",name,ready.wstring()},base,15000);}catch(...){}});
            bool ready_seen=false,rejected=false;
            auto deadline=GetTickCount64()+5000;
            while(!(ready_seen=fs::exists(ready))&&GetTickCount64()<deadline)Sleep(10);
            if(ready_seen){try{UpgradeLock other(name);}catch(...){rejected=true;}}
            std::ofstream(ready.wstring()+L".release")<<"release";child.join();
            require(ready_seen&&rejected&&child_result==0,"Second process was not rejected while upgrade lock held");
            UpgradeLock after(name);
            say(L"PASS cross-process upgrade lock and release");
        }
        auto source=base/L"new 中文 & %literal% !",target=base/L"old 中文 & %literal% !";
        fs::create_directory(source);fs::create_directory(target);fs::create_directory(source/L"nested");
        std::ofstream(source/L"config.json")<<"new config";std::ofstream(source/L"nested"/L"all-content.txt")<<"all content";std::ofstream(target/L"old-only.txt")<<"old backup";
        validate_paths(resolved_path(source),resolved_path(target));
        for(const auto& candidate:{source,source/L"nested",base}) {
            bool rejected=false;try{validate_paths(resolved_path(source),resolved_path(candidate));}catch(...){rejected=true;}require(rejected,"Overlapping paths accepted");
        }
        auto staged=base/L"staged",backup=base/L"backup",failed=base/L"failed";
        copy_all(source,staged);require(equal_file(source/L"nested"/L"all-content.txt",staged/L"nested"/L"all-content.txt"),"Full copy omitted nested files");
        fs::rename(target,backup);fs::rename(staged,target);
        require(!fs::exists(target/L"old-only.txt")&&fs::exists(backup/L"old-only.txt"),"Full replacement did not preserve old backup");
        fs::rename(target,failed);fs::rename(backup,target);require(read(target/L"old-only.txt")=="old backup","Rollback lost original files");
        auto exe=base/L"hik-sdk-http-bridge.exe";fs::copy_file(argv[2],exe);
        auto parsed=arguments(quote(exe.wstring())+L" service --config "+quote((source/L"config.json").wstring()));
        require(parsed.size()==4&&same(parsed[0],exe),"Quoted service path parsed incorrectly");
        require(identify(exe,parsed)==Kind::Native,"Native PE identification failed");
        parsed[1]=L"service-run";bool rejected=false;try{identify(exe,parsed);}catch(...){rejected=true;}require(rejected,"Native PE misidentified as managed");
        require(json_string("{\"version\":\"" HIK_BRIDGE_VERSION "\"}","version")==HIK_BRIDGE_VERSION,"Version JSON parsing failed");
        say(L"PASS upgrade path validation, Unicode arguments, native identification, full replacement and file rollback");return 0;
    }catch(const std::exception& e){say(L"FAIL "+wide(e.what()));return 1;}
}
