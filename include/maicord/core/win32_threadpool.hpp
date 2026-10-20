#pragma once

// kernel32: the thread pool, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/core/win32_kernel.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#endif

typedef struct _TP_CALLBACK_INSTANCE* PTP_CALLBACK_INSTANCE;
typedef void __stdcall TP_SIMPLE_CALLBACK(PTP_CALLBACK_INSTANCE Instance, PVOID Context);
typedef TP_SIMPLE_CALLBACK* PTP_SIMPLE_CALLBACK;
typedef struct _TP_POOL* PTP_POOL;
typedef struct _TP_CLEANUP_GROUP* PTP_CLEANUP_GROUP;
typedef void __stdcall TP_CLEANUP_GROUP_CANCEL_CALLBACK(PVOID ObjectContext, PVOID CleanupContext);
typedef TP_CLEANUP_GROUP_CANCEL_CALLBACK* PTP_CLEANUP_GROUP_CANCEL_CALLBACK;
typedef struct _TP_WORK* PTP_WORK;
typedef void __stdcall TP_WORK_CALLBACK(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_WORK Work);
typedef TP_WORK_CALLBACK* PTP_WORK_CALLBACK;
typedef struct _TP_TIMER* PTP_TIMER;
typedef void __stdcall TP_TIMER_CALLBACK(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_TIMER Timer);
typedef TP_TIMER_CALLBACK* PTP_TIMER_CALLBACK;

MAICORD_SYS_TYPES_BEGIN
enum _TP_CALLBACK_PRIORITY {
    TP_CALLBACK_PRIORITY_HIGH,
    TP_CALLBACK_PRIORITY_NORMAL,
    TP_CALLBACK_PRIORITY_LOW,
    TP_CALLBACK_PRIORITY_INVALID,
    TP_CALLBACK_PRIORITY_COUNT = TP_CALLBACK_PRIORITY_INVALID
};
typedef _TP_CALLBACK_PRIORITY TP_CALLBACK_PRIORITY;
struct _TP_CALLBACK_ENVIRON_V3 {
    TP_VERSION Version;
    PTP_POOL Pool;
    PTP_CLEANUP_GROUP CleanupGroup;
    PTP_CLEANUP_GROUP_CANCEL_CALLBACK CleanupGroupCancelCallback;
    PVOID RaceDll;
    struct _ACTIVATION_CONTEXT* ActivationContext;
    PTP_SIMPLE_CALLBACK FinalizationCallback;
    union {
        DWORD Flags;
        struct {
            DWORD LongFunction : 1;
            DWORD Persistent : 1;
            DWORD Private : 30;
        } s;
    } u;
    TP_CALLBACK_PRIORITY CallbackPriority;
    DWORD Size;
};
typedef _TP_CALLBACK_ENVIRON_V3 TP_CALLBACK_ENVIRON_V3;
typedef TP_CALLBACK_ENVIRON_V3* PTP_CALLBACK_ENVIRON;
MAICORD_SYS_TYPES_END

// <threadpoolapiset.h>
extern "C" __declspec(dllimport) PTP_WORK WINAPI CreateThreadpoolWork(PTP_WORK_CALLBACK pfnwk, PVOID pv,
    PTP_CALLBACK_ENVIRON pcbe);
extern "C" __declspec(dllimport) void WINAPI SubmitThreadpoolWork(PTP_WORK pwk);
extern "C" __declspec(dllimport) void WINAPI WaitForThreadpoolWorkCallbacks(PTP_WORK pwk, BOOL fCancelPendingCallbacks);
extern "C" __declspec(dllimport) void WINAPI CloseThreadpoolWork(PTP_WORK pwk);
extern "C" __declspec(dllimport) PTP_TIMER WINAPI CreateThreadpoolTimer(PTP_TIMER_CALLBACK pfnti, PVOID pv,
    PTP_CALLBACK_ENVIRON pcbe);
extern "C" __declspec(dllimport) void WINAPI SetThreadpoolTimer(PTP_TIMER pti, PFILETIME pftDueTime, DWORD msPeriod,
    DWORD msWindowLength);
extern "C" __declspec(dllimport) void WINAPI WaitForThreadpoolTimerCallbacks(PTP_TIMER pti,
    BOOL fCancelPendingCallbacks);
extern "C" __declspec(dllimport) void WINAPI CloseThreadpoolTimer(PTP_TIMER pti);
