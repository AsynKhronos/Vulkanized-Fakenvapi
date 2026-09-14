#pragma once

#include "openxr_abi.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

class OpenXRHooks {
public:
    // Hooks an already loaded OpenXR loader. Late loading is handled by the
    // shared LoadLibrary watcher in vulkan_hooks.cpp.
    static void initialize(HMODULE openxr_module = nullptr);
    static void shutdown();

    static xrabi::XrResult WINAPI hkxrGetInstanceProcAddr(
        xrabi::XrInstance instance,
        const char* name,
        xrabi::PFN_xrVoidFunction* function);
    static xrabi::XrResult WINAPI hkxrCreateSession(
        xrabi::XrInstance instance,
        const xrabi::XrSessionCreateInfo* create_info,
        xrabi::XrSession* session);
    static xrabi::XrResult WINAPI hkxrWaitFrame(
        xrabi::XrSession session,
        const xrabi::XrFrameWaitInfo* frame_wait_info,
        xrabi::XrFrameState* frame_state);
    static xrabi::XrResult WINAPI hkxrBeginFrame(
        xrabi::XrSession session,
        const xrabi::XrFrameBeginInfo* frame_begin_info);
    static xrabi::XrResult WINAPI hkxrEndFrame(
        xrabi::XrSession session,
        const xrabi::XrFrameEndInfo* frame_end_info);
    static xrabi::XrResult WINAPI hkxrDestroySession(xrabi::XrSession session);
};
