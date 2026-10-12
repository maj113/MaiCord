#ifndef MAICORD_OPUS_CUSTOM_SUPPORT_H
#define MAICORD_OPUS_CUSTOM_SUPPORT_H
/* MaiCord integration: all decoder state is provided by the caller.
 * Opus's allocation-based convenience APIs cannot acquire storage here.
 * memcpy/memmove/memset resolve to MaiCord's freestanding runtime.
 * USE_ALLOCA selects per-call MSVC intrinsic stack scratch, never global state.
 */
#define OVERRIDE_OPUS_ALLOC
#define OVERRIDE_OPUS_REALLOC
#define OVERRIDE_OPUS_FREE
#define OVERRIDE_OPUS_ALLOC_SCRATCH
#define opus_alloc(bytes) ((void)(bytes), (void*)0)
#define opus_realloc(pointer, bytes) ((void)(pointer), (void)(bytes), (void*)0)
#define opus_free(pointer) ((void)(pointer))
#define opus_alloc_scratch(bytes) ((void)(bytes), (void*)0)
#endif
