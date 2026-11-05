#include "maicord/core/heap.hpp"
#include "maicord/core/log.hpp"
#include "maicord/media/compression/deflate.hpp"
#include "gpu/gles/internal.hpp"

namespace maicord {
namespace {

using namespace gles;

// One shader of the pack tools/build_gles_shader_pack.py writes: a zlib stream of GLSL ES 3.10 text at `offset`
// of the packed blob, and `binding_count` manifest rows from `binding_first` of the bindings table.
struct GlesPackedShader {
    u32 offset, packed_bytes, raw_bytes, binding_first;
    u16 binding_count;
    u8 stage; // 0 vertex, 1 pixel, 2 compute
    u8 reserved;
};
#include "maicord_shader_pack_gles.inc"

#define MAICORD_SHADER_ORDER(name) \
    static_assert(kGlesShaderIndex_##name == static_cast<u32>(ShaderId::name), "GLSL shader pack order")
MAICORD_SHADER_ORDER(ui_vertex); MAICORD_SHADER_ORDER(ui_pixel);
MAICORD_SHADER_ORDER(glyph_vertex); MAICORD_SHADER_ORDER(glyph_pixel);
MAICORD_SHADER_ORDER(media_vertex); MAICORD_SHADER_ORDER(media_pixel);
MAICORD_SHADER_ORDER(glyph_compute); MAICORD_SHADER_ORDER(glyph_culled_compute);
MAICORD_SHADER_ORDER(colr_compute); MAICORD_SHADER_ORDER(colr_wide_compute);
MAICORD_SHADER_ORDER(colr_layers_compute); MAICORD_SHADER_ORDER(png_compute);
MAICORD_SHADER_ORDER(vp8_reconstruct); MAICORD_SHADER_ORDER(vp8_idct);
MAICORD_SHADER_ORDER(vp8_filter); MAICORD_SHADER_ORDER(vp8_atlas);
MAICORD_SHADER_ORDER(jpeg_idct); MAICORD_SHADER_ORDER(jpeg_atlas);
MAICORD_SHADER_ORDER(video_frame);
MAICORD_SHADER_ORDER(fx_vertex); MAICORD_SHADER_ORDER(fx_builtin); MAICORD_SHADER_ORDER(fx_target_vertex);
MAICORD_SHADER_ORDER(fx_composite_vertex); MAICORD_SHADER_ORDER(fx_composite_pixel);
#undef MAICORD_SHADER_ORDER
// The pack ends with the variants only this backend uses (tools/build_gles_shader_pack.py GLES_VARIANTS).
static_assert(sizeof(kGlesShaders) / sizeof(kGlesShaders[0]) == static_cast<u32>(ShaderId::count) + 3);

// colr_layers evaluates a pixel's paint plan in 8 slices, a 512-invocation group; a device that cannot run that many
// invocations in a group gets the 2-slice variant (128, the least OpenGL ES 3.1 guarantees), which paints the same.
constexpr u32 kColrFullGroupInvocations = 512;

u32 pack_index(ShaderId id) noexcept {
    if (id == ShaderId::colr_layers_compute && gles::device().caps.max_compute_invocations < kColrFullGroupInvocations)
        return kGlesShaderIndex_colr_layers_narrow_compute;
    return static_cast<u32>(id);
}

constexpr u32 kUnknown = 0xFFFFFFFFu;

// Inflates a packed GLSL text into heap scratch (deflate workspace, then the text and its terminator) and runs
// `use(text, length)`; the driver copies the source, so nothing stays resident.
template <typename Use>
GpuStatus inflate_and_use(const u8* packed, u32 packed_bytes, u32 raw_bytes, Use use) noexcept {
    auto* workspace = static_cast<DeflateWorkspace*>(heap_allocate(sizeof(DeflateWorkspace) + raw_bytes + 1));
    if (!workspace)
        return kGpuOutOfMemory;
    char* text = reinterpret_cast<char*>(workspace + 1);
    const DeflateInputSpan input{packed, packed_bytes};
    GpuStatus result = kGpuFail;
    if (zlib_inflate({&input, 1}, {reinterpret_cast<u8*>(text), raw_bytes}, raw_bytes, workspace).ok()) {
        text[raw_bytes] = 0;
        result = use(static_cast<const char*>(text), raw_bytes);
    } else {
        log_write(LogLevel::error, "gpu", "a packed shader does not inflate");
    }
    heap_free(workspace);
    return result;
}

// The GLSL of pack entry `index`, which must be of `stage`.
template <typename Use>
GpuStatus with_source(u32 index, u8 stage, const GlesPackedShader** out_entry, Use use) noexcept {
    const GlesPackedShader& entry = kGlesShaders[index];
    if (entry.stage != stage || entry.raw_bytes == 0 || entry.raw_bytes > kGlesShaderMaximumBytes)
        return kGpuInvalidArgument;
    *out_entry = &entry;
    return inflate_and_use(kGlesShaderPacked + entry.offset, entry.packed_bytes, entry.raw_bytes, use);
}

void log_info(const gl::Api& gl, bool program, u32 object, const char* what) noexcept {
    char text[1024];
    gl::Sizei length = 0;
    if (program)
        gl.glGetProgramInfoLog(object, sizeof(text) - 1, &length, text);
    else
        gl.glGetShaderInfoLog(object, sizeof(text) - 1, &length, text);
    text[length < 0 ? 0 : length] = 0;
    log_write(LogLevel::error, "gpu", what);
    log_write(LogLevel::error, "gpu", text);
}

} // namespace

namespace gles {

// Compiles `text` as a shader of `type` (a GL shader object in *name).
GpuStatus compile_text(GpuDevice* device, gl::Enum type, const char* text, u32 length, u32 identity, u32* name) noexcept {
    gl::Api& gl = device->gl;
    *name = gl.glCreateShader(type);
    const gl::Int bytes = static_cast<gl::Int>(length);
    gl.glShaderSource(*name, 1, &text, &bytes);
    gl.glCompileShader(*name);
    device->trim_pending = true;
    gl::Int compiled = 0;
    gl.glGetShaderiv(*name, gl::kCompileStatus, &compiled);
    if (compiled)
        return kGpuOk;
    log_info(gl, false, *name, "a shader did not compile");
    log_u64(LogLevel::error, "gpu", "shader_id", identity);
    gl.glDeleteShader(*name);
    *name = 0;
    return kGpuFail;
}

// One shader of the pack by table index (a stage 0 vertex or 1 pixel shader).
GpuStatus create_packed(GpuDevice* device, u32 index, gl::Enum type, u32* name, const GlesPackedShader** entry) noexcept {
    return with_source(index, type == gl::kVertexShader ? 0 : 1, entry, [&](const char* text, u32 length) noexcept {
        return compile_text(device, type, text, length, index, name);
    });
}

GpuStatus create_shader(GpuDevice* device, ShaderId id, gl::Enum type, GlesShader* shader) noexcept {
    if (id >= ShaderId::count)
        return kGpuInvalidArgument;
    const GlesPackedShader* entry = nullptr;
    u32 name = 0;
    const GpuStatus status = create_packed(device, pack_index(id), type, &name, &entry);
    if (!gpu_ok(status))
        return status;
    shader->name = name;
    shader->rows = kGlesBindings + entry->binding_first;
    shader->row_count = entry->binding_count;
    shader->id = id;
    return kGpuOk;
}

// fx_composite_pixel without dual-source blending: its weight and colour halves, which share its manifest.
GpuStatus create_composite_halves(GpuDevice* device, GlesShader* shader) noexcept {
    const GlesPackedShader* entry = nullptr;
    GpuStatus status = create_packed(device, kGlesShaderIndex_fx_composite_weight_pixel, gl::kFragmentShader,
                                     &shader->weight_name, &entry);
    if (gpu_ok(status))
        status = create_packed(device, kGlesShaderIndex_fx_composite_color_pixel, gl::kFragmentShader,
                               &shader->color_name, &entry);
    if (!gpu_ok(status)) {
        if (shader->weight_name)
            device->gl.glDeleteShader(shader->weight_name);
        shader->weight_name = shader->color_name = 0;
        return status;
    }
    shader->name = 0;
    shader->rows = kGlesBindings + entry->binding_first;
    shader->row_count = entry->binding_count;
    shader->id = ShaderId::fx_composite_pixel;
    return kGpuOk;
}

// The lines the GLSL of a package may begin a directive with: what the shader tool writes and nothing else.
bool allowed_directive(const char* line, u32 length) noexcept {
    static constexpr const char* kAllowed[] = {
        "#version 310 es",
        "#extension GL_EXT_control_flow_attributes : require",
        "#extension GL_EXT_blend_func_extended : require",
        "#if defined(GL_EXT_control_flow_attributes)",
        "#define SPIRV_CROSS_FLATTEN [[flatten]]",
        "#define SPIRV_CROSS_BRANCH [[dont_flatten]]",
        "#define SPIRV_CROSS_UNROLL [[unroll]]",
        "#define SPIRV_CROSS_LOOP [[dont_unroll]]",
        "#define SPIRV_CROSS_FLATTEN",
        "#define SPIRV_CROSS_BRANCH",
        "#define SPIRV_CROSS_UNROLL",
        "#define SPIRV_CROSS_LOOP",
        "#else",
        "#endif",
    };
    for (const char* allowed : kAllowed) {
        u32 index = 0;
        while (index < length && allowed[index] && allowed[index] == line[index])
            ++index;
        if (index == length && !allowed[index])
            return true;
    }
    return false;
}

bool has_word(const char* text, u32 length, const char* word) noexcept {
    u32 size = 0;
    while (word[size])
        ++size;
    const auto identifier = [](char c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };
    for (u32 at = 0; at + size <= length; ++at) {
        u32 k = 0;
        while (k < size && text[at + k] == word[k])
            ++k;
        if (k == size && (at == 0 || !identifier(text[at - 1])) && (at + size == length || !identifier(text[at + size])))
            return true;
    }
    return false;
}

// The lexical check of package GLSL before the driver sees it: ASCII, no line continuations, no directives but the
// shader tool's, and none of the words a draw shader needs for storage access (the package host binds none).
bool glsl_text_acceptable(const char* text, u32 length) noexcept {
    for (u32 at = 0; at < length; ++at) {
        const u8 c = static_cast<u8>(text[at]);
        if (c == '\\' || c == 0 || c >= 0x80 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t'))
            return false;
    }
    u32 start = 0;
    while (start < length) {
        u32 end = start;
        while (end < length && text[end] != '\n')
            ++end;
        u32 first = start, last = end;
        while (first < last && (text[first] == ' ' || text[first] == '\t'))
            ++first;
        while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r'))
            --last;
        if (first < last && text[first] == '#' && !allowed_directive(text + first, last - first))
            return false;
        start = end + 1;
    }
    static constexpr const char* kForbidden[] = {"buffer",    "imageStore", "imageLoad", "image2D", "atomicAdd",
                                                 "atomicMin", "atomicMax",  "shared",    "coherent"};
    for (const char* word : kForbidden)
        if (has_word(text, length, word))
            return false;
    return true;
}

// A package shader: the container (glsl_shader.hpp) checked, its manifest copied, its text checked and compiled.
GpuStatus create_blob_shader(GpuDevice* device, ConstSlice<u8> payload, u8 stage, GlesShader* shader) noexcept {
    GlslEntry entry;
    if (!glsl_entry_parse(payload, &entry) || entry.stage != stage)
        return kGpuInvalidArgument;
    for (u32 row = 0; row < entry.binding_count; ++row) {
        const u8* bytes = entry.bindings + row * 4;
        const bool texture = bytes[0] == static_cast<u8>(GlesBindingKind::texture);
        if (bytes[0] != static_cast<u8>(GlesBindingKind::uniform_buffer) && !texture)
            return kGpuInvalidArgument; // storage and image bindings are for the built-in compute shaders
        if (bytes[1] >= kGlesResourceSlots || bytes[2] >= kGlesUnits ||
            (texture ? bytes[3] != kGlesNoSampler && bytes[3] >= kGlesResourceSlots : bytes[2] >= kGlesUniformBindings * 2))
            return kGpuInvalidArgument;
        shader->owned_rows[row] = {bytes[0], bytes[1], bytes[2], bytes[3]};
    }
    u32 name = 0;
    const gl::Enum type = stage == kGlslStageVertex ? gl::kVertexShader : gl::kFragmentShader;
    const GpuStatus status =
        inflate_and_use(entry.packed, entry.packed_bytes, entry.raw_bytes, [&](const char* text, u32 length) noexcept {
            if (!glsl_text_acceptable(text, length)) {
                log_write(LogLevel::error, "gpu", "a package shader holds text the host does not accept");
                return kGpuInvalidArgument;
            }
            return compile_text(device, type, text, length, 0xFFFFu, &name);
        });
    if (!gpu_ok(status))
        return status;
    shader->name = name;
    shader->rows = shader->owned_rows;
    shader->row_count = entry.binding_count;
    shader->id = ShaderId::count;
    return kGpuOk;
}

GpuStatus create_compute_program(GpuDevice* device, ShaderId id, GlesShader* shader) noexcept {
    if (id >= ShaderId::count)
        return kGpuInvalidArgument;
    const GlesPackedShader* entry = nullptr;
    gl::Api& gl = device->gl;
    u32 program = 0;
    const GpuStatus status = with_source(pack_index(id), 2, &entry, [&](const char* text, u32 length) noexcept {
        const u32 object = gl.glCreateShader(gl::kComputeShader);
        const gl::Int bytes = static_cast<gl::Int>(length);
        gl.glShaderSource(object, 1, &text, &bytes);
        gl.glCompileShader(object);
        device->trim_pending = true;
        gl::Int compiled = 0;
        gl.glGetShaderiv(object, gl::kCompileStatus, &compiled);
        if (!compiled) {
            log_info(gl, false, object, "a compute shader did not compile");
            log_u64(LogLevel::error, "gpu", "shader_id", static_cast<u32>(id));
            gl.glDeleteShader(object);
            return kGpuFail;
        }
        program = gl.glCreateProgram();
        gl.glAttachShader(program, object);
        gl.glLinkProgram(program);
        gl.glDetachShader(program, object);
        gl.glDeleteShader(object);
        gl::Int linked = 0;
        gl.glGetProgramiv(program, gl::kLinkStatus, &linked);
        if (!linked) {
            log_info(gl, true, program, "a compute program did not link");
            log_u64(LogLevel::error, "gpu", "shader_id", static_cast<u32>(id));
            gl.glDeleteProgram(program);
            program = 0;
            return kGpuFail;
        }
        return kGpuOk;
    });
    if (!gpu_ok(status))
        return status;
    shader->name = program;
    shader->rows = kGlesBindings + entry->binding_first;
    shader->row_count = entry->binding_count;
    shader->id = id;
    return kGpuOk;
}

} // namespace gles

GpuStatus gpu_vertex_shader_create(GpuDevice* device, ShaderId id, ConstSlice<GpuInputElement> elements,
                                   GpuVertexShader** shader, GpuInputLayout** layout, bool* layout_failed) {
    *shader = nullptr;
    if (layout)
        *layout = nullptr;
    if (layout_failed)
        *layout_failed = false;
    GpuVertexShader* result = device->vertex_shaders.allocate();
    if (!result)
        return kGpuOutOfMemory;
    GpuStatus status = create_shader(device, id, gl::kVertexShader, result);
    if (!gpu_ok(status)) {
        device->vertex_shaders.release(result);
        return status;
    }
    GpuInputLayout* created = nullptr;
    if (elements.count != 0) {
        created = device->layouts.allocate();
        gl::Api& gl = device->gl;
        if (!created) {
            status = kGpuOutOfMemory;
        } else {
            // One vertex buffer binding (0), every attribute a per-instance vec4 at its record offset; the
            // attribute location is the element's position, which the shader tool assigns the same way.
            gl.glGenVertexArrays(1, &created->vertex_array);
            gl.glBindVertexArray(created->vertex_array);
            for (u32 index = 0; index < elements.count; ++index) {
                gl.glEnableVertexAttribArray(index);
                gl.glVertexAttribFormat(index, 4, gl::kFloat, gl::kFalse, elements[index].offset);
                gl.glVertexAttribBinding(index, 0);
            }
            gl.glVertexBindingDivisor(0, 1);
            gl.glBindVertexArray(0);
            gles::context().applied_vertex_array = kUnknown;
            created->count = elements.count;
            status = gles::check_error("input layout creation");
            if (!gpu_ok(status)) {
                gl.glDeleteVertexArrays(1, &created->vertex_array);
                device->layouts.release(created);
                created = nullptr;
            }
        }
        if (!gpu_ok(status)) {
            if (layout_failed)
                *layout_failed = true;
            GpuVertexShader* failed = result;
            gpu_release(failed);
            return status;
        }
    }
    *shader = result;
    if (layout)
        *layout = created;
    else
        gpu_release(created);
    return kGpuOk;
}

GpuStatus gpu_pixel_shader_create(GpuDevice* device, ShaderId id, GpuPixelShader** shader) {
    *shader = nullptr;
    GpuPixelShader* result = device->pixel_shaders.allocate();
    if (!result)
        return kGpuOutOfMemory;
    // The composite's second output is dual-source blending; without it the shader is its two halves.
    const GpuStatus status = id == ShaderId::fx_composite_pixel && !device->caps.dual_source_blend
                                 ? create_composite_halves(device, result)
                                 : create_shader(device, id, gl::kFragmentShader, result);
    if (!gpu_ok(status)) {
        device->pixel_shaders.release(result);
        return status;
    }
    *shader = result;
    return kGpuOk;
}

GpuStatus gpu_vertex_shader_create_blob(GpuDevice* device, ConstSlice<u8> payload, GpuVertexShader** shader) {
    *shader = nullptr;
    GpuVertexShader* result = device->vertex_shaders.allocate();
    if (!result)
        return kGpuOutOfMemory;
    const GpuStatus status = create_blob_shader(device, payload, kGlslStageVertex, result);
    if (!gpu_ok(status)) {
        device->vertex_shaders.release(result);
        return status;
    }
    *shader = result;
    return kGpuOk;
}

GpuStatus gpu_pixel_shader_create_blob(GpuDevice* device, ConstSlice<u8> payload, GpuPixelShader** shader) {
    *shader = nullptr;
    GpuPixelShader* result = device->pixel_shaders.allocate();
    if (!result)
        return kGpuOutOfMemory;
    const GpuStatus status = create_blob_shader(device, payload, kGlslStagePixel, result);
    if (!gpu_ok(status)) {
        device->pixel_shaders.release(result);
        return status;
    }
    *shader = result;
    return kGpuOk;
}

GpuStatus gpu_compute_shader_create(GpuDevice* device, ShaderId id, GpuComputeShader** shader) {
    *shader = nullptr;
    GpuComputeShader* result = device->compute_shaders.allocate();
    if (!result)
        return kGpuOutOfMemory;
    const GpuStatus status = create_compute_program(device, id, result);
    if (!gpu_ok(status)) {
        device->compute_shaders.release(result);
        return status;
    }
    *shader = result;
    return kGpuOk;
}

namespace {

// Deletes the cached programs that link the released shader, so a reused slab slot never matches a stale pair.
void drop_programs(GpuDevice& device, const void* shader) noexcept {
    u32 kept = 0;
    for (u32 index = 0; index < device.program_count; ++index) {
        const GlesProgram& program = device.programs[index];
        if (program.vertex == shader || program.pixel == shader) {
            if (program.name)
                device.gl.glDeleteProgram(program.name);
            gles::context().applied_program = kUnknown;
        } else {
            device.programs[kept++] = program;
        }
    }
    device.program_count = kept;
}

template <typename Slab>
bool in_slab(const Slab& slab, const void* pointer) noexcept {
    const u8* start = reinterpret_cast<const u8*>(slab.items);
    const u8* at = static_cast<const u8*>(pointer);
    return at >= start && at < start + sizeof(slab.items);
}

} // namespace

void gpu_release(GpuVertexShader*& handle) noexcept {
    GpuVertexShader* shader = handle;
    handle = nullptr;
    if (!shader)
        return;
    // The pass executor keeps a graph's vertex and pixel shaders in one union and releases each through the vertex
    // overload (on Direct3D 11 one Release does for both): a pixel shader arriving here goes to its own slab.
    if (in_slab(gles::device().pixel_shaders, shader)) {
        GpuPixelShader* pixel = reinterpret_cast<GpuPixelShader*>(shader);
        gpu_release(pixel);
        return;
    }
    GpuDevice& d = gles::device();
    drop_programs(d, shader);
    d.gl.glDeleteShader(shader->name);
    if (gles::context().vertex_shader == shader)
        gles::context().vertex_shader = nullptr;
    d.vertex_shaders.release(shader);
}

void gpu_release(GpuPixelShader*& handle) noexcept {
    GpuPixelShader* shader = handle;
    handle = nullptr;
    if (!shader)
        return;
    if (in_slab(gles::device().vertex_shaders, shader)) {
        GpuVertexShader* vertex = reinterpret_cast<GpuVertexShader*>(shader);
        gpu_release(vertex);
        return;
    }
    GpuDevice& d = gles::device();
    drop_programs(d, shader);
    if (shader->name)
        d.gl.glDeleteShader(shader->name);
    if (shader->weight_name)
        d.gl.glDeleteShader(shader->weight_name);
    if (shader->color_name)
        d.gl.glDeleteShader(shader->color_name);
    if (gles::context().pixel_shader == shader)
        gles::context().pixel_shader = nullptr;
    d.pixel_shaders.release(shader);
}

void gpu_release(GpuComputeShader*& handle) noexcept {
    GpuComputeShader* shader = handle;
    handle = nullptr;
    if (!shader)
        return;
    GpuDevice& d = gles::device();
    d.gl.glDeleteProgram(shader->name);
    GpuContext& c = gles::context();
    if (c.compute_shader == shader)
        c.compute_shader = nullptr;
    c.applied_program = kUnknown;
    d.compute_shaders.release(shader);
}

} // namespace maicord
