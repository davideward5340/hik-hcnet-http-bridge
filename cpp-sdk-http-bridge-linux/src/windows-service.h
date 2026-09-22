#pragma once

#include <functional>

// This header and its implementation are used only by the Windows target.
int run_windows_service(const std::function<int()>& run, void (*stop)());
void windows_service_start_progress();
void windows_service_ready();
void windows_service_error(const char* message);
