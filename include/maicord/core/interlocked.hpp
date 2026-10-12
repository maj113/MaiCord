#pragma once

// Declared as <intrin.h> and <winnt.h> declare them, without parsing either.
extern "C" long _InterlockedCompareExchange(long volatile* destination, long exchange, long comparand);
extern "C" long _InterlockedExchange(long volatile* target, long value);
extern "C" long _InterlockedIncrement(long volatile* addend);
#pragma intrinsic(_InterlockedCompareExchange, _InterlockedExchange, _InterlockedIncrement)
