#pragma once

// kernel32: the thread pool, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/core/win32_kernel.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#endif

typedef struct _TP_CALLBACK_INSTANCE* PTP_CALLBACK_INSTANCE;
typedef struct _TP_WORK* PTP_WORK;
typedef void __stdcall TP_WORK_CALLBACK(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_WORK Work);
typedef TP_WORK_CALLBACK* PTP_WORK_CALLBACK;
typedef struct _TP_TIMER* PTP_TIMER;
typedef void __stdcall TP_TIMER_CALLBACK(PTP_CALLBACK_INSTANCE Instance, PVOID Context, PTP_TIMER Timer);
typedef TP_TIMER_CALLBACK* PTP_TIMER_CALLBACK;

typedef struct _TP_CALLBACK_ENVIRON_V3* PTP_CALLBACK_ENVIRON;

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
