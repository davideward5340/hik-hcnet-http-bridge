// Native command doubles for CMD tests. Never call the real SCM or registry.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
namespace fs = std::filesystem;

std::string encode(const std::wstring& value, UINT cp = CP_UTF8) {
    int size = WideCharToMultiByte(cp, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(cp, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring decode(const std::string& value) {
    int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
void output(const std::wstring& value) {
    auto bytes = encode(value, 936);
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}
void save(const fs::path& file, const std::wstring& value) { std::ofstream(file, std::ios::binary) << encode(value); }
std::wstring read(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    return decode(std::string(std::istreambuf_iterator<char>(input), {}));
}

int wmain(int argc, wchar_t** argv) {
    const auto root_value = _wgetenv(L"HIK_CMD_TEST_STATE");
    if (!root_value) return 99;
    const fs::path root(root_value);
    const auto tool = fs::path(argv[0]).filename().wstring();
    std::vector<std::wstring> args(argv + 1, argv + argc);
    {
        std::ofstream log(root / "calls.tsv", std::ios::app | std::ios::binary);
        log << encode(tool);
        for (const auto& arg : args) log << '\t' << encode(arg);
        log << '\n';
    }
    auto flag = [&](const wchar_t* name) { return fs::exists(root / name); };
    auto option = [&](const std::wstring& name) {
        auto it = std::find(args.begin(), args.end(), name);
        return it != args.end() && ++it != args.end() ? *it : std::wstring();
    };
    if (tool == L"fltmc.exe") return flag(L"noadmin") ? 5 : 0;
    if (tool == L"ping.exe") return 0;
    if (tool == L"hik-sdk-http-bridge.exe") return flag(L"badconfig") ? 2 : 0;
    if (tool == L"sc.exe" && args.size() >= 2) {
        const auto service = root / (args[1] + L".service");
        const auto state = root / (args[1] + L".state");
        const auto& command = args[0];
        if (command == L"query") {
            if (flag(L"query-denied")) return 5;
            if (!fs::exists(service)) return 1060;
            output(L"SERVICE_NAME: " + args[1] + L"\r\n        STATE              : " + read(state) + L"  TEST_STATE\r\n");
            return 0;
        }
        if (command == L"create" || command == L"config") {
            if (command == L"create" && fs::exists(service)) return 1073;
            if (command == L"config" && !fs::exists(service)) return 1060;
            save(service, option(L"binPath="));
            if (command == L"create") save(state, L"1");
            return 0;
        }
        if (command == L"start") {
            if (flag(L"fail-start")) { fs::remove(root / L"fail-start"); return 1053; }
            if (!fs::exists(service)) return 1060;
            save(state, L"4");
            return 0;
        }
        if (command == L"stop") { save(state, L"1"); return 0; }
        if (command == L"delete") {
            if (!flag(L"pending-delete")) { fs::remove(service); fs::remove(state); }
            return 0;
        }
        if (command == L"failure" && flag(L"fail-recovery")) return 5;
        if (command == L"failure" || command == L"failureflag") return 0;
    }
    if (tool == L"reg.exe" && !args.empty()) {
        if (args[0] == L"export") { save(fs::path(args[2]), L"test backup"); return 0; }
        if (args[0] == L"query") {
            const auto value = option(L"/v");
            if (value == L"ImagePath") output(L"    ImagePath    REG_EXPAND_SZ    " + read(root / L"hikbridge.service") + L"\r\n");
            else if (value == L"Start") output(L"    Start    REG_DWORD    0x2\r\n");
            else if (value == L"DelayedAutoStart") output(L"    DelayedAutoStart    REG_DWORD    0x1\r\n");
            else return 1;
            return 0;
        }
    }
    if (tool == L"schtasks.exe" && !args.empty()) {
        const auto task = root / (option(L"/TN") + L".task");
        if (args[0] == L"/Query") {
            if (!fs::exists(task)) return 1;
            if (std::find(args.begin(), args.end(), L"/XML") != args.end()) output(read(task));
            return 0;
        }
        if (args[0] == L"/Create") { save(task, L"<Task>restored</Task>"); return 0; }
        if (args[0] == L"/Delete") { fs::remove(task); return 0; }
        if (args[0] == L"/Change" || args[0] == L"/End" || args[0] == L"/Run") return 0;
    }
    output(L"Unexpected mocked command\r\n");
    return 98;
}
