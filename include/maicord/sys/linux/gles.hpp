#pragma once

// OpenGL ES 3.1 (and the 3.2 and extension values the GPU backend probes) as libGLESv2.so.2 declares it, without
// <GLES3/gl32.h>: the types, the values and the table of entry points the backend loads at run time
// (maicord/gpu/gles). Add what you use together with its entry in the table.

#include "maicord/core/base.hpp"

namespace maicord::gl {

using Enum = u32;
using Bitfield = u32;
using Int = i32;
using Uint = u32;
using Sizei = i32;
using Boolean = u8;
using Sizeiptr = i64;
using Intptr = i64;
using Sync = void*; // a GLsync
inline constexpr Enum kSyncGpuCommandsComplete = 0x9117;
inline constexpr Enum kAlreadySignaled = 0x911A, kConditionSatisfied = 0x911C;
inline constexpr Bitfield kSyncFlushCommandsBit = 0x00000001;
inline constexpr Enum kTimestampExt = 0x8E28, kQueryResultAvailableExt = 0x8867, kQueryResultExt = 0x8866,
                      kGpuDisjointExt = 0x8FBB;

inline constexpr Boolean kFalse = 0;
inline constexpr Enum kZero = 0;
inline constexpr Enum kOne = 1;
inline constexpr Enum kNoError = 0;

// Capabilities and queries.
inline constexpr Enum kVendor = 0x1F00;
inline constexpr Enum kRenderer = 0x1F01;
inline constexpr Enum kVersion = 0x1F02;
inline constexpr Enum kExtensions = 0x1F03;
inline constexpr Enum kNumExtensions = 0x821D;
inline constexpr Enum kMajorVersion = 0x821B;
inline constexpr Enum kMinorVersion = 0x821C;
inline constexpr Enum kMaxTextureSize = 0x0D33;
inline constexpr Enum kMaxUniformBlockSize = 0x8A30;
inline constexpr Enum kMaxComputeWorkGroupInvocations = 0x90EB;
inline constexpr Enum kMaxComputeSharedMemorySize = 0x8262;
inline constexpr Enum kMaxComputeShaderStorageBlocks = 0x90DB;
inline constexpr Enum kScissorTest = 0x0C11;
inline constexpr Enum kBlend = 0x0BE2;
inline constexpr Enum kCullFace = 0x0B44;
inline constexpr Enum kDither = 0x0BD0;
inline constexpr Enum kDepthTest = 0x0B71;

// Buffers.
inline constexpr Enum kUniformBuffer = 0x8A11;
inline constexpr Enum kShaderStorageBuffer = 0x90D2;
inline constexpr Enum kCopyWriteBuffer = 0x8F37;
inline constexpr Enum kStaticDraw = 0x88E4;
inline constexpr Enum kDynamicDraw = 0x88E8;
inline constexpr Bitfield kMapWriteBit = 0x0002;
inline constexpr Bitfield kMapInvalidateBufferBit = 0x0008;
inline constexpr Bitfield kMapUnsynchronizedBit = 0x0020;

// Textures and images.
inline constexpr Enum kTexture2d = 0x0DE1;
inline constexpr Enum kTexture0 = 0x84C0;
inline constexpr Enum kTextureMinFilter = 0x2801;
inline constexpr Enum kTextureMagFilter = 0x2800;
inline constexpr Enum kTextureWrapS = 0x2802;
inline constexpr Enum kTextureWrapT = 0x2803;
inline constexpr Enum kTextureSwizzleR = 0x8E42;
inline constexpr Enum kTextureSwizzleB = 0x8E44;
inline constexpr Enum kTextureBaseLevel = 0x813C;
inline constexpr Enum kTextureMaxLevel = 0x813D;
inline constexpr Enum kNearest = 0x2600;
inline constexpr Enum kLinear = 0x2601;
inline constexpr Enum kClampToEdge = 0x812F;
inline constexpr Enum kRepeat = 0x2901;
inline constexpr Enum kRed = 0x1903;
inline constexpr Enum kBlue = 0x1905;
inline constexpr Enum kRgba = 0x1908;
inline constexpr Enum kRg = 0x8227;
inline constexpr Enum kUnsignedByte = 0x1401;
inline constexpr Enum kHalfFloat = 0x140B;
inline constexpr Enum kFloat = 0x1406;
inline constexpr Enum kR8 = 0x8229;
inline constexpr Enum kRg8 = 0x822B;
inline constexpr Enum kRgba8 = 0x8058;
inline constexpr Enum kRgba16f = 0x881A;
inline constexpr Enum kRgba32f = 0x8814;
inline constexpr Enum kReadWrite = 0x88BA;
inline constexpr Enum kUnpackAlignment = 0x0CF5;
inline constexpr Enum kUnpackRowLength = 0x0CF2;
inline constexpr Enum kPackAlignment = 0x0D05;
inline constexpr Enum kPackRowLength = 0x0D02;

// Framebuffers.
inline constexpr Enum kFramebuffer = 0x8D40;
inline constexpr Enum kReadFramebuffer = 0x8CA8;
inline constexpr Enum kDrawFramebuffer = 0x8CA9;
inline constexpr Enum kColorAttachment0 = 0x8CE0;
inline constexpr Enum kFramebufferComplete = 0x8CD5;
inline constexpr Enum kColor = 0x1800;
inline constexpr Bitfield kColorBufferBit = 0x00004000;

// Shaders, programs and vertex state.
inline constexpr Enum kFragmentShader = 0x8B30;
inline constexpr Enum kVertexShader = 0x8B31;
inline constexpr Enum kComputeShader = 0x91B9;
inline constexpr Enum kCompileStatus = 0x8B81;
inline constexpr Enum kLinkStatus = 0x8B82;
inline constexpr Enum kTriangleStrip = 0x0005;

// Blending.
inline constexpr Enum kFuncAdd = 0x8006;
inline constexpr Enum kSrcColor = 0x0300;
inline constexpr Enum kOneMinusSrcColor = 0x0301;
inline constexpr Enum kSrcAlpha = 0x0302;
inline constexpr Enum kOneMinusSrcAlpha = 0x0303;
inline constexpr Enum kConstantColor = 0x8001;
inline constexpr Enum kConstantAlpha = 0x8003;
inline constexpr Enum kSrc1Color = 0x88F9; // GL_SRC1_COLOR_EXT (EXT_blend_func_extended)

// Barriers.
inline constexpr Bitfield kAllBarrierBits = 0xFFFFFFFFu;

// The functions the backend loads from libGLESv2.so.2 by name (ES 3.1; glCopyImageSubData is 3.2 or an
// extension and is reached through eglGetProcAddress as GlesApi::glCopyImageSubData when present).
#define MAICORD_GLES_FUNCTIONS(X)                                                                                      \
    X(Enum, glGetError, ())                                                                                            \
    X(const u8*, glGetString, (Enum name))                                                                             \
    X(const u8*, glGetStringi, (Enum name, Uint index))                                                                \
    X(void, glGetIntegerv, (Enum name, Int * values))                                                                  \
    X(void, glEnable, (Enum capability))                                                                               \
    X(void, glDisable, (Enum capability))                                                                              \
    X(void, glViewport, (Int x, Int y, Sizei width, Sizei height))                                                     \
    X(void, glScissor, (Int x, Int y, Sizei width, Sizei height))                                                      \
    X(void, glFlush, ())                                                                                               \
    X(void, glPixelStorei, (Enum name, Int value))                                                                     \
    X(void, glBlendFuncSeparate, (Enum source, Enum destination, Enum source_alpha, Enum destination_alpha))           \
    X(void, glBlendEquationSeparate, (Enum color, Enum alpha))                                                         \
    X(void, glBlendColor, (float red, float green, float blue, float alpha))                                           \
    X(void, glGenBuffers, (Sizei count, Uint * names))                                                                 \
    X(void, glDeleteBuffers, (Sizei count, const Uint* names))                                                         \
    X(void, glBindBuffer, (Enum target, Uint name))                                                                    \
    X(void, glBindBufferBase, (Enum target, Uint index, Uint name))                                                    \
    X(void, glBufferData, (Enum target, Sizeiptr bytes, const void* data, Enum usage))                                 \
    X(void, glBufferSubData, (Enum target, Intptr offset, Sizeiptr bytes, const void* data))                           \
    X(void*, glMapBufferRange, (Enum target, Intptr offset, Sizeiptr bytes, Bitfield access))                          \
    X(Boolean, glUnmapBuffer, (Enum target))                                                                           \
    X(void, glGenTextures, (Sizei count, Uint * names))                                                                \
    X(void, glDeleteTextures, (Sizei count, const Uint* names))                                                        \
    X(void, glBindTexture, (Enum target, Uint name))                                                                   \
    X(void, glActiveTexture, (Enum unit))                                                                              \
    X(void, glTexStorage2D, (Enum target, Sizei levels, Enum internal_format, Sizei width, Sizei height))              \
    X(void, glTexSubImage2D, (Enum target, Int level, Int x, Int y, Sizei width, Sizei height, Enum format,            \
                              Enum type, const void* pixels))                                                          \
    X(void, glTexParameteri, (Enum target, Enum name, Int value))                                                      \
    X(void, glBindImageTexture, (Uint unit, Uint texture, Int level, Boolean layered, Int layer, Enum access,          \
                                 Enum format))                                                                         \
    X(void, glReadPixels, (Int x, Int y, Sizei width, Sizei height, Enum format, Enum type, void* pixels))             \
    X(void, glGenFramebuffers, (Sizei count, Uint * names))                                                            \
    X(void, glDeleteFramebuffers, (Sizei count, const Uint* names))                                                    \
    X(void, glBindFramebuffer, (Enum target, Uint name))                                                               \
    X(void, glFramebufferTexture2D, (Enum target, Enum attachment, Enum texture_target, Uint texture, Int level))      \
    X(Enum, glCheckFramebufferStatus, (Enum target))                                                                   \
    X(void, glReadBuffer, (Enum source))                                                                               \
    X(void, glBlitFramebuffer, (Int source_x0, Int source_y0, Int source_x1, Int source_y1, Int destination_x0,        \
                                Int destination_y0, Int destination_x1, Int destination_y1, Bitfield mask,             \
                                Enum filter))                                                                          \
    X(void, glClearBufferfv, (Enum buffer, Int draw_buffer, const float* value))                                       \
    X(void, glGenSamplers, (Sizei count, Uint * names))                                                                \
    X(void, glDeleteSamplers, (Sizei count, const Uint* names))                                                        \
    X(void, glBindSampler, (Uint unit, Uint name))                                                                     \
    X(void, glSamplerParameteri, (Uint name, Enum parameter, Int value))                                               \
    X(Uint, glCreateShader, (Enum type))                                                                               \
    X(void, glShaderSource, (Uint shader, Sizei count, const char* const* sources, const Int* lengths))                \
    X(void, glCompileShader, (Uint shader))                                                                            \
    X(void, glGetShaderiv, (Uint shader, Enum name, Int * value))                                                      \
    X(void, glGetShaderInfoLog, (Uint shader, Sizei capacity, Sizei * length, char* log))                              \
    X(void, glDeleteShader, (Uint shader))                                                                             \
    X(Uint, glCreateProgram, ())                                                                                       \
    X(void, glAttachShader, (Uint program, Uint shader))                                                               \
    X(void, glDetachShader, (Uint program, Uint shader))                                                               \
    X(void, glLinkProgram, (Uint program))                                                                             \
    X(void, glGetProgramiv, (Uint program, Enum name, Int * value))                                                    \
    X(void, glGetProgramInfoLog, (Uint program, Sizei capacity, Sizei * length, char* log))                            \
    X(void, glUseProgram, (Uint program))                                                                              \
    X(void, glDeleteProgram, (Uint program))                                                                           \
    X(void, glGenVertexArrays, (Sizei count, Uint * names))                                                            \
    X(void, glDeleteVertexArrays, (Sizei count, const Uint* names))                                                    \
    X(void, glBindVertexArray, (Uint name))                                                                            \
    X(void, glEnableVertexAttribArray, (Uint index))                                                                   \
    X(void, glVertexAttribFormat, (Uint index, Int size, Enum type, Boolean normalized, Uint relative_offset))         \
    X(void, glVertexAttribBinding, (Uint index, Uint binding))                                                         \
    X(void, glVertexBindingDivisor, (Uint binding, Uint divisor))                                                      \
    X(void, glBindVertexBuffer, (Uint binding, Uint buffer, Intptr offset, Sizei stride))                              \
    X(void, glDrawArrays, (Enum mode, Int first, Sizei count))                                                         \
    X(void, glDrawArraysInstanced, (Enum mode, Int first, Sizei count, Sizei instances))                               \
    X(void, glDispatchCompute, (Uint x, Uint y, Uint z))                                                               \
    X(Sync, glFenceSync, (Enum condition, Bitfield flags))                                                             \
    X(Enum, glClientWaitSync, (Sync sync, Bitfield flags, u64 timeout))                                                \
    X(void, glDeleteSync, (Sync sync))                                                                                 \
    X(void, glMemoryBarrier, (Bitfield barriers))

struct Api {
#define MAICORD_GLES_MEMBER(result, name, arguments) result(*name) arguments;
    MAICORD_GLES_FUNCTIONS(MAICORD_GLES_MEMBER)
#undef MAICORD_GLES_MEMBER
    // ES 3.2, GL_OES_copy_image or GL_EXT_copy_image, through eglGetProcAddress: device creation refuses a driver with none.
    void (*glCopyImageSubData)(Uint source, Enum source_target, Int source_level, Int sx, Int sy, Int sz,
                               Uint destination, Enum destination_target, Int destination_level, Int dx, Int dy,
                               Int dz, Sizei width, Sizei height, Sizei depth);
    // Optional: GL_EXT_disjoint_timer_query; null otherwise.
    void (*glGenQueriesEXT)(Sizei count, Uint* names);
    void (*glDeleteQueriesEXT)(Sizei count, const Uint* names);
    void (*glQueryCounterEXT)(Uint name, Enum target);
    void (*glGetQueryObjectuivEXT)(Uint name, Enum parameter, Uint* value);
    void (*glGetQueryObjectui64vEXT)(Uint name, Enum parameter, u64* value);
};

} // namespace maicord::gl
