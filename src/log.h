#pragma once
#include <iostream>
#include <fstream>
#include <format>
#include <iomanip>
#include <windows.h>
#include <dxgi.h>
#include "nvapi_mingw_compat.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "util.h"
#include "config.h"

#define OK() Ok(__func__)
#undef ERROR
#define ERROR() Error(__func__)
#define ERROR_VALUE(status) Error(__func__, status)

// Frame-hot diagnostics are useful in debug builds but must disappear entirely
// from release binaries: no level check, formatting, timestamp or logger call.
#ifdef NDEBUG
#define VFN_HOT_TRACE(...) do { } while (0)
#else
#define VFN_HOT_TRACE(...) spdlog::trace(__VA_ARGS__)
#endif

NvAPI_Status Ok(const char* function_name);
NvAPI_Status Error(const char* function_name, NvAPI_Status status = NVAPI_ERROR);
void prepare_logging(spdlog::level::level_enum level, std::string path);
void close_logging();

void log_pcl(double pcl);

template <typename... _Args>
void log_event(const char* event_name, std::format_string<_Args...> __fmt, _Args&&... __args) {
    VFN_HOT_TRACE("EVENT,{},{},{}", event_name, get_timestamp(), std::vformat(__fmt.get(), std::make_format_args(__args...)));
}
