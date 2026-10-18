#pragma once

// Compiler intrinsics and SIMD vector types, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/crt.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <intrin.h>
#endif

// <intrin0.inl.h>
extern "C" unsigned char _BitScanForward(unsigned long* _Index, unsigned long _Mask);
extern "C" unsigned char _BitScanForward64(unsigned long* _Index, unsigned __int64 _Mask);
extern "C" unsigned char _BitScanReverse(unsigned long* _Index, unsigned long _Mask);
extern "C" unsigned char _BitScanReverse64(unsigned long* _Index, unsigned __int64 _Mask);
extern "C" long _InterlockedCompareExchange(long volatile* _Destination, long _Exchange, long _Comparand);
extern "C" __int64 _InterlockedCompareExchange64(__int64 volatile* _Destination, __int64 _Exchange, __int64 _Comparand);
extern "C" long _InterlockedDecrement(long volatile* _Addend);
extern "C" long _InterlockedExchange(long volatile* _Target, long _Value);
extern "C" __int64 _InterlockedExchange64(__int64 volatile* _Target, __int64 _Value);
extern "C" long _InterlockedExchangeAdd(long volatile* _Addend, long _Value);
extern "C" __int64 _InterlockedExchangeAdd64(__int64 volatile* _Addend, __int64 _Value);
extern "C" long _InterlockedIncrement(long volatile* _Addend);
extern "C" __int64 _InterlockedIncrement64(__int64 volatile* _Addend);
extern "C" long _InterlockedOr(long volatile* _Value, long _Mask);
extern "C" void _ReadWriteBarrier();
extern "C" void _mm_pause();
extern "C" unsigned __int64 __popcnt64(unsigned __int64);

// <intrin.h>
extern "C" void* _InterlockedCompareExchangePointer(void* volatile* _Destination, void* _Exchange, void* _Comparand);
extern "C" void* _InterlockedExchangePointer(void* volatile* _Target, void* _Value);
extern "C" void* _ReturnAddress();
extern "C" void __cpuid(int[4], int);
extern "C" void __cdecl __debugbreak();
extern "C" __declspec(noreturn) void __fastfail(unsigned int);
extern "C" unsigned __int64 __rdtsc();
extern "C" unsigned long __readgsdword(unsigned long);
extern "C" unsigned __int64 __readgsqword(unsigned long);
extern "C" void __stosb(unsigned char*, unsigned char, size_t);
extern "C" [[nodiscard]] unsigned __int64 __cdecl _byteswap_uint64(unsigned __int64);
extern "C" [[nodiscard]] unsigned long __cdecl _byteswap_ulong(unsigned long);
extern "C" [[nodiscard]] unsigned short __cdecl _byteswap_ushort(unsigned short);
