#pragma once

// NVIDIA's legacy SAL shim is not nesting-safe when nvapi.h includes the
// nvapi_lite headers under MinGW.  Keep NVIDIA's empty SAL annotations alive
// only while nvapi.h is parsed, then run its cleanup pass once at the wrapper
// boundary so tokens such as __in/__out cannot leak into the C++ standard
// library.
#if defined(__MINGW32__) || defined(__MINGW64__)
#ifndef __NVAPI_EMPTY_SAL
#define __NVAPI_EMPTY_SAL 1
#define VULKANIZED_NVAPI_DEFINED_EMPTY_SAL 1
#endif

#ifdef __success
#pragma push_macro("__success")
#undef __success
#define VULKANIZED_NVAPI_RESTORE_SUCCESS 1
#endif
#endif

#include <nvapi.h>

#if defined(VULKANIZED_NVAPI_DEFINED_EMPTY_SAL)
// nvapi.h's normal salend includes intentionally did nothing while
// __NVAPI_EMPTY_SAL was set.  Remove the nesting guard and execute one final
// cleanup pass now.  This undefines only the annotations NVIDIA created and
// their __nvapi_* bookkeeping macros.
#undef __NVAPI_EMPTY_SAL
#undef VULKANIZED_NVAPI_DEFINED_EMPTY_SAL
#include <nvapi_lite_salend.h>
#endif

#if defined(VULKANIZED_NVAPI_RESTORE_SUCCESS)
#pragma pop_macro("__success")
#undef VULKANIZED_NVAPI_RESTORE_SUCCESS
#endif
