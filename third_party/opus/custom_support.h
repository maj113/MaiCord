#ifndef MAIOPUS_CUSTOM_SUPPORT_H
#define MAIOPUS_CUSTOM_SUPPORT_H
/* Included by os_support.h when CUSTOM_SUPPORT is defined. Codec states live in
 * caller memory, so the allocation hooks never return storage; scratch uses
 * per-call alloca (USE_ALLOCA), never global state. */
#define OVERRIDE_OPUS_ALLOC
#define OVERRIDE_OPUS_REALLOC
#define OVERRIDE_OPUS_FREE
#define OVERRIDE_OPUS_ALLOC_SCRATCH
#define opus_alloc(bytes) ((void)(bytes), (void*)0)
#define opus_realloc(pointer, bytes) ((void)(pointer), (void)(bytes), (void*)0)
#define opus_free(pointer) ((void)(pointer))
#define opus_alloc_scratch(bytes) ((void)(bytes), (void*)0)
#endif
