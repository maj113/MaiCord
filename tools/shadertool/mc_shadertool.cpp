// mc_shadertool: SPIR-V from DXC to GLSL ES 3.10 plus the binding manifest of the GLES backend. A host build tool
// (it links SPIRV-Cross and the C++ library, so it is not part of any MaiCord image); tools/glsl_translate.py runs it
// once per shader of a plugin effect (the client's own shaders are hand-written GLSL, src/render/shaders/gles). The
// conventions are those of design-gpu.md section 4.3, and the hand-written GLSL follows the same ones:
//
//   * DXC is told to put each register class in its own binding range (-fvk-t-shift 32, -fvk-s-shift 64,
//     -fvk-u-shift 96; b registers keep their number), which is how this tool tells a texture from a buffer.
//   * Uniform buffer b#: GL binding = # for a vertex or compute shader, 8 + # for a pixel shader.
//   * Textures: every (texture t#, sampler s#) pair a shader uses becomes one combined sampler. The first pair of a
//     texture takes unit #; further pairs take units from 16. Load-only textures pair with SPIRV-Cross's dummy
//     sampler (sampler slot 255).
//   * Storage buffers and images take bindings in order of (t slots, then u slots) from 0, below the ES 3.1 limit
//     of four storage blocks per compute stage that the shaders were checked against.
//   * Vertex shaders get the Y flip and the clip-space fix-up that give D3D11's picture and depth range, a pixel
//     shader gets highp default precision, and GL_EXT_blend_func_extended is required when an output has index 1.
//
// usage: mc_shadertool INPUT.spv vs|ps|cs OUTPUT.glsl OUTPUT.manifest
// The manifest has one line per resource: "kind slot binding sampler" (see GlesBindingKind in src/gpu/gles).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "spirv_glsl.hpp"

using namespace spirv_cross;

namespace {

// GlesBindingKind of src/gpu/gles/internal.hpp.
enum Kind { kUniformBuffer = 0, kTexture = 1, kStorageSrv = 2, kStorageUav = 3, kImage = 4 };

constexpr uint32_t kTextureBase = 32, kSamplerBase = 64, kUnorderedBase = 96;
constexpr uint32_t kExtraUnitBase = 16;
constexpr uint32_t kPixelUniformBase = 8;
constexpr unsigned kNoSampler = 255;

struct Row {
    unsigned kind, slot, binding, sampler;
};

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "mc_shadertool: %s\n", message.c_str());
    std::exit(1);
}

std::vector<uint32_t> read_words(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        fail(std::string("cannot read ") + path);
    std::vector<char> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() % 4 != 0)
        fail("not a SPIR-V module");
    std::vector<uint32_t> words(bytes.size() / 4);
    std::copy(bytes.begin(), bytes.end(), reinterpret_cast<char*>(words.data()));
    return words;
}

uint32_t binding_of(const CompilerGLSL& compiler, ID id, uint32_t base, uint32_t limit, const char* what) {
    const uint32_t binding = compiler.get_decoration(id, spv::DecorationBinding);
    if (binding < base || binding >= base + limit)
        fail(std::string(what) + " has binding " + std::to_string(binding) + " outside its register range");
    return binding - base;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5)
        fail("usage: mc_shadertool INPUT.spv vs|ps|cs OUTPUT.glsl OUTPUT.manifest");
    const std::string stage = argv[2];
    if (stage != "vs" && stage != "ps" && stage != "cs")
        fail("the stage is vs, ps or cs");
    try {
        CompilerGLSL compiler(read_words(argv[1]));

        CompilerGLSL::Options options = compiler.get_common_options();
        options.es = true;
        options.version = 310;
        options.vertex.flip_vert_y = stage == "vs";
        options.vertex.fixup_clipspace = stage == "vs";
        // Draws always start at instance 0 (the backend rebinds the vertex buffer instead of a base instance).
        options.vertex.support_nonzero_base_instance = false;
        options.fragment.default_float_precision = CompilerGLSL::Options::Highp;
        options.fragment.default_int_precision = CompilerGLSL::Options::Highp;
        compiler.set_common_options(options);

        const uint32_t dummy_sampler = compiler.build_dummy_sampler_for_combined_images();
        if (dummy_sampler != 0) {
            compiler.set_decoration(dummy_sampler, spv::DecorationDescriptorSet, 0);
            compiler.set_decoration(dummy_sampler, spv::DecorationBinding, 0);
        }
        compiler.build_combined_image_samplers();

        std::vector<Row> rows;
        const ShaderResources resources = compiler.get_shader_resources();

        for (const Resource& buffer : resources.uniform_buffers) {
            const uint32_t slot = binding_of(compiler, buffer.id, 0, kTextureBase, "a constant buffer");
            const uint32_t binding = slot + (stage == "ps" ? kPixelUniformBase : 0);
            compiler.set_decoration(buffer.id, spv::DecorationBinding, binding);
            compiler.set_decoration(buffer.id, spv::DecorationDescriptorSet, 0);
            // A block name is shared across the stages of a program, and the same name must mean the same
            // binding: the stages bind one HLSL slot to different GL bindings, so each stage names its blocks.
            compiler.set_name(buffer.base_type_id, "type_" + buffer.name + "_" + stage);
            rows.push_back({kUniformBuffer, slot, binding, kNoSampler});
        }

        // The first combination of a texture takes its own unit, the others the next free extra units.
        std::map<uint32_t, bool> first_taken;
        uint32_t next_extra = kExtraUnitBase;
        for (const CombinedImageSampler& combined : compiler.get_combined_image_samplers()) {
            const uint32_t texture = binding_of(compiler, combined.image_id, kTextureBase, kSamplerBase - kTextureBase,
                                                "a texture");
            unsigned sampler = kNoSampler;
            if (combined.sampler_id != dummy_sampler)
                sampler = binding_of(compiler, combined.sampler_id, kSamplerBase, kUnorderedBase - kSamplerBase,
                                     "a sampler");
            uint32_t unit;
            if (!first_taken[texture]) {
                first_taken[texture] = true;
                unit = texture;
            } else {
                unit = next_extra++;
            }
            compiler.set_decoration(combined.combined_id, spv::DecorationBinding, unit);
            compiler.set_decoration(combined.combined_id, spv::DecorationDescriptorSet, 0);
            compiler.set_name(combined.combined_id,
                              "t" + std::to_string(texture) + (sampler == kNoSampler ? "" : "_s" + std::to_string(sampler)));
            rows.push_back({kTexture, texture, unit, sampler});
        }

        struct Storage {
            ID id;
            unsigned kind;
            uint32_t slot;
        };
        std::vector<Storage> storage;
        for (const Resource& buffer : resources.storage_buffers) {
            const uint32_t raw = compiler.get_decoration(buffer.id, spv::DecorationBinding);
            if (raw >= kUnorderedBase)
                storage.push_back({buffer.id, kStorageUav, raw - kUnorderedBase});
            else
                storage.push_back({buffer.id, kStorageSrv, binding_of(compiler, buffer.id, kTextureBase,
                                                                     kSamplerBase - kTextureBase, "a buffer")});
        }
        std::stable_sort(storage.begin(), storage.end(), [](const Storage& left, const Storage& right) {
            return left.kind != right.kind ? left.kind < right.kind : left.slot < right.slot;
        });
        uint32_t next_storage = 0;
        for (const Storage& buffer : storage) {
            compiler.set_decoration(buffer.id, spv::DecorationBinding, next_storage);
            compiler.set_decoration(buffer.id, spv::DecorationDescriptorSet, 0);
            rows.push_back({buffer.kind, buffer.slot, next_storage, kNoSampler});
            ++next_storage;
        }

        std::vector<Storage> images;
        for (const Resource& image : resources.storage_images)
            images.push_back({image.id, kImage, binding_of(compiler, image.id, kUnorderedBase, 32, "an image")});
        std::stable_sort(images.begin(), images.end(),
                         [](const Storage& left, const Storage& right) { return left.slot < right.slot; });
        uint32_t next_image = 0;
        for (const Storage& image : images) {
            compiler.set_decoration(image.id, spv::DecorationBinding, next_image);
            compiler.set_decoration(image.id, spv::DecorationDescriptorSet, 0);
            rows.push_back({kImage, image.slot, next_image, kNoSampler});
            ++next_image;
        }

        for (const Resource& output : resources.stage_outputs)
            if (compiler.has_decoration(output.id, spv::DecorationIndex) &&
                compiler.get_decoration(output.id, spv::DecorationIndex) != 0)
                compiler.require_extension("GL_EXT_blend_func_extended");

        const std::string glsl = compiler.compile();
        std::ofstream(argv[3], std::ios::binary) << glsl;
        std::ofstream manifest(argv[4], std::ios::binary);
        for (const Row& row : rows)
            manifest << row.kind << ' ' << row.slot << ' ' << row.binding << ' ' << row.sampler << '\n';
    } catch (const CompilerError& error) {
        fail(std::string("SPIRV-Cross: ") + error.what());
    }
    return 0;
}
