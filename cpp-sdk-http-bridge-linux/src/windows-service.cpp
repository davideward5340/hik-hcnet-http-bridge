#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "windows-service.h"
#include <mutex>
#include <atomic>
#include <thread>

namespace {
#ifndef HIK_SERVICE_NAME
#define HIK_SERVICE_NAME L"hikbridge"
#endif
constexpr wchar_t service_name[] = HIK_SERVICE_NAME;
std::mutex status_mutex;
SERVICE_STATUS_HANDLE status_handle = nullptr;
SERVICE_STATUS status{};
std::function<int()> run_callback;
void (*stop_callback)() = nullptr;
int result_code = 1;
std::atomic<ULONGLONG> stop_started{0};

// Caller holds status_mutex. Never report RUNNING after a stop request.
void report(DWORD state, DWORD error = NO_ERROR) {
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwWin32ExitCode = error == NO_ERROR ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
    status.dwServiceSpecificExitCode = error;
    const bool pending = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING;
    status.dwCheckPoint = pending ? status.dwCheckPoint + 1 : 0;
    status.dwWaitHint = pending ? 60000 : 0;
    SetServiceStatus(status_handle, &status);
}

DWORD WINAPI control(DWORD code, DWORD, void*, void*) {
    std::lock_guard<std::mutex> lock(status_mutex);
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) {
        if (status.dwCurrentState == SERVICE_RUNNING) {
            report(SERVICE_STOP_PENDING);
            stop_started.store(GetTickCount64());
            stop_callback();
        }
        return NO_ERROR;
    }
    if (code == SERVICE_CONTROL_INTERROGATE) return NO_ERROR;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI service_main(DWORD, wchar_t**) {
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        status_handle = RegisterServiceCtrlHandlerExW(service_name, control, nullptr);
        if (!status_handle) { result_code = static_cast<int>(GetLastError()); return; }
        report(SERVICE_START_PENDING);
    }
    std::atomic_bool finished{false};
    std::thread worker;
    try {
        worker = std::thread([&] {
            try { result_code = run_callback(); }
            catch (...) { windows_service_error("Unhandled exception in service worker"); result_code = 1; }
            finished.store(true);
        });
        while (!finished.load()) {
            const auto started = stop_started.load();
            if (started && GetTickCount64() - started >= 60000) {
                // A vendor SDK call can hang outside our cancellation mechanism.
                // Do not free SDK state under live workers or leave STOP_PENDING
                // forever. Process teardown also closes the FFmpeg job handles.
                windows_service_error("Service cleanup exceeded 60 seconds; terminating the process");
                TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
            }
            Sleep(100);
        }
        worker.join();
    } catch (...) { windows_service_error("Cannot start service worker"); result_code = 1; }
    std::lock_guard<std::mutex> lock(status_mutex);
    report(SERVICE_STOPPED, static_cast<DWORD>(result_code));
    status_handle = nullptr;
}
}

int run_windows_service(const std::function<int()>& run, void (*stop)()) {
    run_callback = run;
    stop_callback = stop;
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t*>(service_name), service_main}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        const auto error = GetLastError();
        windows_service_error("Cannot connect to SCM. Use 'run' for foreground execution.");
        return static_cast<int>(error);
    }
    return result_code;
}

void windows_service_start_progress() {
    std::lock_guard<std::mutex> lock(status_mutex);
    if (status_handle && status.dwCurrentState == SERVICE_START_PENDING) report(SERVICE_START_PENDING);
}

void windows_service_ready() {
    std::lock_guard<std::mutex> lock(status_mutex);
    if (status_handle && status.dwCurrentState == SERVICE_START_PENDING) report(SERVICE_RUNNING);
}

void windows_service_error(const char* message) {
    // Also works before configuration and file logging have been initialized.
    HANDLE source = RegisterEventSourceA(nullptr, "hikbridge");
    if (source) {
        ReportEventA(source, EVENTLOG_ERROR_TYPE, 0, 1, nullptr, 1, 0, &message, nullptr);
        DeregisterEventSource(source);
    }
}
