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
static_assert(sizeof(kGlesShaders) / sizeof(kGlesShaders[0]) == static_cast<u32>(ShaderId::count));

constexpr u32 kUnknown = 0xFFFFFFFFu;

// Inflates the GLSL of `id` into heap scratch (deflate workspace, then the text and its terminator) and runs
// `use(text, length)`; the driver copies the source, so nothing stays resident.
template <typename Use>
GpuStatus with_source(ShaderId id, u8 stage, const GlesPackedShader** out_entry, Use use) noexcept {
    if (id >= ShaderId::count)
        return kGpuInvalidArgument;
    const GlesPackedShader& entry = kGlesShaders[static_cast<u32>(id)];
    if (entry.stage != stage || entry.raw_bytes == 0 || entry.raw_bytes > kGlesShaderMaximumBytes)
        return kGpuInvalidArgument;
    *out_entry = &entry;
    auto* workspace = static_cast<DeflateWorkspace*>(heap_allocate(sizeof(DeflateWorkspace) + entry.raw_bytes + 1));
    if (!workspace)
        return kGpuOutOfMemory;
    char* text = reinterpret_cast<char*>(workspace + 1);
    const DeflateInputSpan input{kGlesShaderPacked + entry.offset, entry.packed_bytes};
    GpuStatus result = kGpuFail;
    if (zlib_inflate({&input, 1}, {reinterpret_cast<u8*>(text), entry.raw_bytes}, entry.raw_bytes, workspace).ok()) {
        text[entry.raw_bytes] = 0;
        result = use(static_cast<const char*>(text), entry.raw_bytes);
    } else {
        log_write(LogLevel::error, "gpu", "a packed shader does not inflate");
    }
    heap_free(workspace);
    return result;
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

GpuStatus create_shader(GpuDevice* device, ShaderId id, gl::Enum type, GlesShader* shader) noexcept {
    const u8 stage = type == gl::kVertexShader ? 0 : 1;
    const GlesPackedShader* entry = nullptr;
    gl::Api& gl = device->gl;
    u32 name = 0;
    const GpuStatus status = with_source(id, stage, &entry, [&](const char* text, u32 length) noexcept {
        name = gl.glCreateShader(type);
        const gl::Int bytes = static_cast<gl::Int>(length);
        gl.glShaderSource(name, 1, &text, &bytes);
        gl.glCompileShader(name);
        gl::Int compiled = 0;
        gl.glGetShaderiv(name, gl::kCompileStatus, &compiled);
        if (!compiled) {
            log_info(gl, false, name, "a shader did not compile");
            log_u64(LogLevel::error, "gpu", "shader_id", static_cast<u32>(id));
            gl.glDeleteShader(name);
            name = 0;
            return kGpuFail;
        }
        return kGpuOk;
    });
    if (!gpu_ok(status))
        return status;
    shader->name = name;
    shader->rows = kGlesBindings + entry->binding_first;
    shader->row_count = entry->binding_count;
    shader->id = id;
    return kGpuOk;
}

GpuStatus create_compute_program(GpuDevice* device, ShaderId id, GlesShader* shader) noexcept {
    const GlesPackedShader* entry = nullptr;
    gl::Api& gl = device->gl;
    u32 program = 0;
    const GpuStatus status = with_source(id, 2, &entry, [&](const char* text, u32 length) noexcept {
        const u32 object = gl.glCreateShader(gl::kComputeShader);
        const gl::Int bytes = static_cast<gl::Int>(length);
        gl.glShaderSource(object, 1, &text, &bytes);
        gl.glCompileShader(object);
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
    *layout = created;
    return kGpuOk;
}

GpuStatus gpu_pixel_shader_create(GpuDevice* device, ShaderId id, GpuPixelShader** shader) {
    *shader = nullptr;
    GpuPixelShader* result = device->pixel_shaders.allocate();
    if (!result)
        return kGpuOutOfMemory;
    const GpuStatus status = create_shader(device, id, gl::kFragmentShader, result);
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

} // namespace

void gpu_release(GpuVertexShader*& handle) noexcept {
    GpuVertexShader* shader = handle;
    handle = nullptr;
    if (!shader)
        return;
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
    GpuDevice& d = gles::device();
    drop_programs(d, shader);
    d.gl.glDeleteShader(shader->name);
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
