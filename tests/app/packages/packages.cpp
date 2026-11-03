#include "maicord/app/packages/install.hpp"
#include "maicord/app/packages/link.hpp"
#include "maicord/app/packages/mcfx.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_user.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace {

using Check = void (*)(bool, const char*) noexcept;

McfxWorkspace workspace;
u8 file[kMcfxMaximumBytes + 64];
u8 mutated[kMcfxMaximumBytes + 64];
u8 big[kMcfxBytecodeMaximumBytes + 8];

struct Section {
    u32 type;
    const u8* data;
    u32 bytes;
};

void put16(u8* p, u32 v) noexcept { p[0] = static_cast<u8>(v), p[1] = static_cast<u8>(v >> 8); }
void put32(u8* p, u32 v) noexcept { put16(p, v & 0xffff), put16(p + 2, v >> 16); }

// A container of `sections` packed back to back, as the packager writes it.
u32 build(const Section* sections, u32 count) noexcept {
    clear_bytes(file, sizeof(file));
    copy_bytes(file, "MCFX", 4);
    put16(file + 4, 1);
    put16(file + 6, count);
    u32 at = 16 + count * 16;
    for (u32 i = 0; i < count; ++i) {
        put32(file + 16 + i * 16, sections[i].type);
        put32(file + 16 + i * 16 + 4, at);
        put32(file + 16 + i * 16 + 8, sections[i].bytes);
        copy_bytes(file + at, sections[i].data, sections[i].bytes);
        at += sections[i].bytes;
    }
    put32(file + 8, at);
    return at;
}

// DXBC with one SHEX chunk whose version token is `version` (0x50: a ps_5_0 pixel shader).
u8 dxbc[48];
void make_dxbc(u32 version) noexcept {
    clear_bytes(dxbc, sizeof(dxbc));
    copy_bytes(dxbc, "DXBC", 4);
    put32(dxbc + 20, 1);
    put32(dxbc + 24, sizeof(dxbc));
    put32(dxbc + 28, 1);
    put32(dxbc + 32, 36);
    copy_bytes(dxbc + 36, "SHEX", 4);
    put32(dxbc + 40, 4);
    put32(dxbc + 44, version);
}

constexpr char kManifest[] =
    "{\"interface\":1,\"slug\":\"rain-drops\",\"name\":\"Rain drops\",\"version\":\"1.2.0\","
    "\"authors\":[{\"name\":\"Maj\",\"github\":\"maj113\"},{\"name\":\"Lumen\"}],\"license\":\"MPL-2.0\","
    "\"derived_from\":{\"url\":\"https://example.org/rain\",\"author\":\"Someone\",\"license\":\"MIT\"},"
    "\"blend\":\"screen\",\"uses\":[\"noise\",\"glyphs\"],\"resolution\":75,\"fps\":30,\"knobs\":["
    "{\"id\":\"speed\",\"type\":\"float\",\"label\":\"Speed\",\"min\":0,\"max\":3,\"default\":1},"
    "{\"id\":\"count\",\"type\":\"int\",\"label\":\"Count\",\"min\":1,\"max\":8,\"default\":4},"
    "{\"id\":\"mirror\",\"type\":\"toggle\",\"label\":\"Mirror\",\"default\":true},"
    "{\"id\":\"mode\",\"type\":\"choice\",\"label\":\"Mode\",\"choices\":[\"Calm\",\"Storm\"],\"default\":1},"
    "{\"id\":\"tint\",\"type\":\"color\",\"label\":\"Tint\",\"default\":\"#1aff59\"},"
    "{\"id\":\"gain\",\"type\":\"float\",\"label\":\"Gain\",\"min\":-1,\"max\":1,\"default\":0.25}]}";

u32 good_package(const char* manifest = kManifest) noexcept {
    make_dxbc(0x50);
    const Section sections[]{{1, reinterpret_cast<const u8*>(manifest), c_string_length(manifest)},
                             {2, dxbc, sizeof(dxbc)},
                             {5, reinterpret_cast<const u8*>("float4 background(float2 p){return 0;}"), 38}};
    return build(sections, 3);
}

bool parses(u32 bytes) noexcept { return mcfx_parse({file, bytes}, &workspace).ok(); }
McfxError error_of(u32 bytes) noexcept { return mcfx_parse({file, bytes}, &workspace).error; }

bool text_is(const char* a, const char* b) noexcept {
    const u32 n = c_string_length(b);
    return c_string_length(a) == n && bytes_equal(a, b, n);
}

void test_parse(Check check) noexcept {
    u32 bytes = good_package();
    const auto result = mcfx_parse({file, bytes}, &workspace);
    const auto& p = workspace.package;
    check(result.ok() && text_is(p.slug, "rain-drops") && text_is(p.name, "Rain drops") && text_is(p.version, "1.2.0") &&
              text_is(p.license, "MPL-2.0") && text_is(p.authors, "Maj, Lumen") && p.has_credit &&
              text_is(p.derived_from.author, "Someone") && p.blend == EffectBlend::screen &&
              p.uses == (effect_use_noise | effect_use_glyphs) && p.requested_resolution == 4 &&
              p.requested_frame_rate == 4 && p.source.count == 38 && p.shaders[0].count == sizeof(dxbc),
          "mcfx: a complete package parses into its manifest fields and borrowed sections");
    // Scalars, toggles and choices pack four to a row in order; a colour takes the next row's .rgb.
    check(p.knob_count == 6 && p.knobs[0].slot == 0 && p.knobs[1].slot == 1 && p.knobs[2].slot == 2 &&
              p.knobs[3].slot == 3 && p.knobs[4].slot == 4 && p.knobs[5].slot == 8 && p.rows == 3 &&
              p.knobs[4].rgb == 0x1aff59 && p.knobs[3].choice_count == 2 && text_is(p.choices[0][1], "Storm"),
          "mcfx: knob slots follow declaration order, colours own a row, rows are counted");
    McfxKnobValue values[kMcfxKnobMaximum];
    float rows[kEffectKnobRows][4];
    mcfx_knob_defaults(p, values);
    mcfx_knob_pack(p, values, rows);
    check(rows[0][0] == 1.f && rows[0][1] == 4.f && rows[0][2] == 1.f && rows[0][3] == 1.f && rows[1][0] > .09f &&
              rows[1][0] < .11f && rows[1][1] == 1.f && rows[1][2] > .34f && rows[1][2] < .36f && rows[2][0] == .25f &&
              rows[3][0] == 0.f,
          "mcfx: defaults pack into mc_knobs (sRGB colours as typed)");
    values[0].number = 5;
    check(!mcfx_knob_value_valid(p.knobs[0], values[0]) && !mcfx_knob_value_valid(p.knobs[1], {2.5f, 0}) &&
              mcfx_knob_value_valid(p.knobs[1], {3.f, 0}) && mcfx_knob_value_valid(p.knobs[4], {0, 0x123456}),
          "mcfx: knob values outside the schema are refused");
}

void test_container(Check check) noexcept {
    const u32 bytes = good_package();
    // Every truncation fails, in the header, the table or a section.
    bool truncations = true;
    for (u32 cut = 0; cut < bytes; ++cut)
        truncations = truncations && !parses(cut);
    check(truncations && parses(bytes), "mcfx: every truncation of a good package is refused");
    check(error_of(0) == McfxError::truncated && error_of(15) == McfxError::truncated, "mcfx: an empty or short file is truncated");
    // Header fields.
    good_package();
    file[0] = 'X';
    check(error_of(bytes) == McfxError::header, "mcfx: bad magic");
    good_package();
    put16(file + 4, 2);
    check(error_of(bytes) == McfxError::header, "mcfx: unknown format version");
    good_package();
    put16(file + 6, 10);
    check(error_of(bytes) == McfxError::header, "mcfx: more than nine sections");
    good_package();
    put32(file + 8, bytes + 1);
    check(error_of(bytes) == McfxError::header, "mcfx: declared size differs from the file");
    good_package();
    put32(file + 12, 1);
    check(error_of(bytes) == McfxError::header, "mcfx: reserved header word must be zero");
    good_package();
    put32(file + 16 + 12, 9);
    check(error_of(bytes) == McfxError::section, "mcfx: reserved table word must be zero");
    // Table: order, unknown, duplicate, gap, overlap.
    good_package();
    put32(file + 16 + 16, 1); // second entry repeats MANIFEST
    check(error_of(bytes) == McfxError::section, "mcfx: a repeated section type is refused");
    good_package();
    put32(file + 16 + 32, 10);
    check(error_of(bytes) == McfxError::section, "mcfx: an unknown section type is refused");
    good_package();
    put32(file + 16 + 4, 0);
    check(!parses(bytes), "mcfx: a section pointing into the header is refused");
    good_package();
    put32(file + 16 + 16 + 4, 16 + 48 + 1); // a gap before the second section
    check(error_of(bytes) == McfxError::section, "mcfx: gaps between sections are refused");
    good_package();
    put32(file + 16 + 8, 0xffffffffu);
    check(!parses(bytes), "mcfx: a section length past the end is refused");
    // Required sections.
    make_dxbc(0x50);
    const Section only_code[]{{2, dxbc, sizeof(dxbc)}};
    check(error_of(build(only_code, 1)) == McfxError::section, "mcfx: the manifest is required");
    const Section only_manifest[]{{1, reinterpret_cast<const u8*>(kManifest), c_string_length(kManifest)}};
    check(error_of(build(only_manifest, 1)) == McfxError::section, "mcfx: the bytecode is required");
    // Size caps.
    clear_bytes(big, sizeof(big));
    const Section too_big[]{{1, reinterpret_cast<const u8*>(kManifest), c_string_length(kManifest)},
                            {2, big, kMcfxBytecodeMaximumBytes + 1}};
    check(error_of(build(too_big, 2)) == McfxError::oversize, "mcfx: bytecode over 64 KiB is refused");
    clear_bytes(mutated, sizeof(mutated));
    check(mcfx_parse({mutated, kMcfxMaximumBytes + 1}, &workspace).error == McfxError::oversize,
          "mcfx: a file over 1.5 MiB is refused before any parsing");
}

u32 with_bytecode(u32 version, u32 patch_at = 0, u32 patch = 0) noexcept {
    make_dxbc(version);
    if (patch_at)
        put32(dxbc + patch_at, patch);
    const Section sections[]{{1, reinterpret_cast<const u8*>(kManifest), c_string_length(kManifest)}, {2, dxbc, sizeof(dxbc)}};
    return build(sections, 2);
}

void test_bytecode(Check check) noexcept {
    check(parses(with_bytecode(0x50)), "mcfx: a ps_5_0 shader container is accepted");
    check(error_of(with_bytecode(0x10050)) == McfxError::bytecode, "mcfx: a vertex shader is refused");
    check(error_of(with_bytecode(0x40)) == McfxError::bytecode, "mcfx: shader model 4.0 is refused");
    check(error_of(with_bytecode(0x50, 24, 47)) == McfxError::bytecode, "mcfx: DXBC total size must equal the section");
    check(error_of(with_bytecode(0x50, 28, 0)) == McfxError::bytecode, "mcfx: DXBC with no chunks is refused");
    check(error_of(with_bytecode(0x50, 28, 1000)) == McfxError::bytecode, "mcfx: DXBC chunk count past the data is refused");
    check(error_of(with_bytecode(0x50, 32, 400)) == McfxError::bytecode, "mcfx: DXBC chunk offset past the data is refused");
    check(error_of(with_bytecode(0x50, 40, 0xfffffff0u)) == McfxError::bytecode, "mcfx: DXBC chunk size past the data is refused");
    check(error_of(with_bytecode(0x50, 36, 0x44585342u)) == McfxError::bytecode, "mcfx: DXBC without a shader chunk is refused");
    make_dxbc(0x50);
    dxbc[0] = 'Z';
    const Section bad_magic[]{{1, reinterpret_cast<const u8*>(kManifest), c_string_length(kManifest)}, {2, dxbc, sizeof(dxbc)}};
    check(error_of(build(bad_magic, 2)) == McfxError::bytecode, "mcfx: bytecode without the DXBC magic is refused");
}

// Replaces `from` with `to` in `base`, into `out` (which must not be `base`).
const char* replace(const char* base, const char* from, const char* to, char* out) noexcept {
    if (!base)
        return nullptr;
    const u32 n = c_string_length(base), f = c_string_length(from), t = c_string_length(to);
    u32 at = 0;
    while (at + f <= n && !bytes_equal(base + at, from, f))
        ++at;
    if (at + f > n)
        return nullptr;
    copy_bytes(out, base, at);
    copy_bytes(out + at, to, t);
    copy_bytes(out + at + t, base + at + f, n - at - f + 1);
    return out;
}
// Replaces `from` with `to` in the good manifest.
char edited[2048];
const char* edit(const char* from, const char* to) noexcept {
    return replace(kManifest, from, to, edited);
}

bool refuses(const char* from, const char* to) noexcept {
    const char* manifest = edit(from, to);
    return manifest && !parses(good_package(manifest)) && error_of(good_package(manifest)) == McfxError::manifest;
}

// The good manifest at `interface` with `uses` in place of its inputs.
char ambient_manifest[2048];
const char* with_inputs(const char* interface, const char* uses) noexcept {
    return replace(edit("\"interface\":1", interface), "\"uses\":[\"noise\",\"glyphs\"]", uses, ambient_manifest);
}
McfxError ambient_error(const char* interface, const char* uses) noexcept {
    const char* manifest = with_inputs(interface, uses);
    return manifest ? error_of(good_package(manifest)) : McfxError::invalid_input;
}

void test_manifest(Check check) noexcept {
    check(refuses("\"interface\":1", "\"interface\":4") && refuses("\"interface\":1", "\"interface\":0") &&
              refuses("\"interface\":1,", "") &&
              refuses("\"slug\":\"rain-drops\"", "\"slug\":\"Rain\"") && refuses("\"slug\":\"rain-drops\"", "\"slug\":\"-x\"") &&
              refuses("\"version\":\"1.2.0\"", "\"version\":\"1 2\"") && refuses("\"license\":\"MPL-2.0\"", "\"license\":\"\""),
          "mcfx manifest: interface, slug, version and license are checked");
    // Interface 2: clock and appearance (mc_ambient) are opt-in names that need it; everything else is unchanged.
    const char* ambient = with_inputs("\"interface\":2", "\"uses\":[\"noise\",\"clock\",\"appearance\"]");
    const bool opted = ambient && parses(good_package(ambient)) &&
                       workspace.package.uses == (effect_use_noise | effect_use_clock | effect_use_appearance);
    const char* plain_two = with_inputs("\"interface\":2", "\"uses\":[\"noise\",\"glyphs\"]");
    const bool two_without = plain_two && parses(good_package(plain_two)) &&
                             workspace.package.uses == (effect_use_noise | effect_use_glyphs);
    check(opted && two_without &&
              ambient_error("\"interface\":1", "\"uses\":[\"clock\"]") == McfxError::manifest &&
              ambient_error("\"interface\":1", "\"uses\":[\"appearance\"]") == McfxError::manifest &&
              ambient_error("\"interface\":2", "\"uses\":[\"clock\",\"clock\"]") == McfxError::manifest &&
              ambient_error("\"interface\":2", "\"uses\":[\"sunlight\"]") == McfxError::manifest,
          "mcfx manifest: clock and appearance parse under interface 2 only; interface 2 alone changes nothing");
    check(parses(good_package()) && workspace.package.uses == (effect_use_noise | effect_use_glyphs) &&
              !(workspace.package.uses & kEffectAmbientUses),
          "mcfx manifest: an interface-1 package parses as before, with no ambient input");
    check(refuses("\"blend\":\"screen\"", "\"blend\":\"multiply\"") && refuses("\"uses\":[\"noise\",\"glyphs\"]", "\"uses\":[\"noise\",\"noise\"]") &&
              refuses("\"uses\":[\"noise\",\"glyphs\"]", "\"uses\":[\"texture0\"]") && refuses("\"resolution\":75", "\"resolution\":80") &&
              refuses("\"fps\":30", "\"fps\":0"),
          "mcfx manifest: blend, inputs, a texture without a section and off-table request values are refused");
    check(refuses("\"interface\":1,", "\"interface\":1,\"extra\":1,") && refuses("\"slug\":\"rain-drops\",", "\"slug\":\"a\",\"slug\":\"b\",") &&
              refuses("\"name\":\"Maj\",", "\"name\":\"Maj\",\"nick\":\"m\",") && refuses("\"github\":\"maj113\"", "\"github\":\"a b\""),
          "mcfx manifest: unknown and duplicate members are refused");
    check(refuses("\"max\":3,\"default\":1", "\"max\":3,\"default\":4") && refuses("\"min\":0,\"max\":3", "\"min\":3,\"max\":3") &&
              refuses("\"min\":1,\"max\":8,\"default\":4", "\"min\":1,\"max\":8,\"default\":4.5") &&
              refuses("\"choices\":[\"Calm\",\"Storm\"]", "\"choices\":[\"Calm\"]") && refuses("\"default\":1},{\"id\":\"tint\"", "\"default\":2},{\"id\":\"tint\"") &&
              refuses("\"default\":\"#1aff59\"", "\"default\":\"1aff59\"") && refuses("\"id\":\"gain\"", "\"id\":\"speed\"") &&
              refuses("\"id\":\"mirror\"", "\"id\":\"Mirror\""),
          "mcfx manifest: knob ranges, defaults, choices, colours and ids are checked");
    const char* percent = edit("\"id\":\"gain\",\"type\":\"float\"", "\"id\":\"gain\",\"type\":\"float\",\"percent\":true");
    check(percent && parses(good_package(percent)) && workspace.package.knobs[5].percent && !workspace.package.knobs[0].percent &&
              refuses("\"id\":\"count\",\"type\":\"int\"", "\"id\":\"count\",\"type\":\"int\",\"percent\":true") &&
              refuses("\"id\":\"gain\",\"type\":\"float\"", "\"id\":\"gain\",\"type\":\"float\",\"percent\":true,\"percent\":false"),
          "mcfx manifest: a float knob may be a percentage; other knobs and a repeated flag are refused");
    // Nine colours need nine rows.
    static char many[4096];
    u32 n = 0;
    const char head[] = "{\"interface\":1,\"slug\":\"x\",\"name\":\"X\",\"version\":\"1\",\"authors\":[{\"name\":\"A\"}],\"license\":\"MIT\","
                        "\"blend\":\"normal\",\"uses\":[],\"knobs\":[";
    copy_bytes(many, head, sizeof(head) - 1);
    n = sizeof(head) - 1;
    for (u32 i = 0; i < 5; ++i) {
        const char colour[] = "{\"id\":\"c0\",\"type\":\"color\",\"label\":\"C\",\"default\":\"#000000\"},";
        copy_bytes(many + n, colour, sizeof(colour) - 1);
        many[n + 8] = static_cast<char>('0' + i);
        n += sizeof(colour) - 1;
    }
    many[n - 1] = ']';
    many[n++] = '}';
    many[n] = 0;
    check(error_of(good_package(many)) == McfxError::manifest, "mcfx manifest: more than four colours are refused");
}

void test_mutation(Check check) noexcept {
    const u32 bytes = good_package();
    // Deterministic byte flips and swaps: nothing crashes, and a package never parses with a smaller size.
    u64 state = 0x9e3779b97f4a7c15ull;
    const auto next = [&]() noexcept {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    u32 accepted = 0;
    for (u32 round = 0; round < 6000; ++round) {
        copy_bytes(mutated, file, bytes);
        const u32 flips = 1 + next() % 3;
        for (u32 i = 0; i < flips; ++i)
            mutated[next() % bytes] = static_cast<u8>(next());
        const u32 length = round % 7 == 0 ? next() % (bytes + 1) : bytes;
        accepted += mcfx_parse({mutated, length}, &workspace).ok();
    }
    check(accepted < 3000, "mcfx: 6,000 mutated packages are parsed without a crash and mostly refused");
    // A rewritten manifest byte never corrupts the neighbouring section.
    good_package();
    check(parses(bytes), "mcfx: the good package is still accepted after the mutation runs");
}

// PNG: an 8-bit RGBA IHDR is all the container reads; the decoder runs when the effect is selected.
u32 make_png(u8* out, u32 width, u32 height, u32 colour) noexcept {
    static const u8 signature[8]{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    copy_bytes(out, signature, 8);
    out[8] = 0, out[9] = 0, out[10] = 0, out[11] = 13;
    copy_bytes(out + 12, "IHDR", 4);
    const u32 values[2]{width, height};
    for (u32 i = 0; i < 2; ++i)
        for (u32 b = 0; b < 4; ++b)
            out[16 + i * 4 + b] = static_cast<u8>(values[i] >> (24 - b * 8));
    out[24] = 8, out[25] = static_cast<u8>(colour), out[26] = out[27] = out[28] = 0;
    return 33;
}

void test_textures(Check check) noexcept {
    static u8 png[64], wide[64], palette[64];
    const u32 png_bytes = make_png(png, 64, 32, 6), wide_bytes = make_png(wide, 513, 8, 6), palette_bytes = make_png(palette, 8, 8, 3);
    const char* manifest = edit("\"uses\":[\"noise\",\"glyphs\"]", "\"uses\":[\"texture0\"]");
    make_dxbc(0x50);
    const Section good[]{{1, reinterpret_cast<const u8*>(manifest), c_string_length(manifest)}, {2, dxbc, sizeof(dxbc)}, {3, png, png_bytes}};
    check(parses(build(good, 3)) && workspace.package.texture[0].count == png_bytes, "mcfx: a declared texture section is accepted");
    const Section oversize[]{{1, reinterpret_cast<const u8*>(manifest), c_string_length(manifest)}, {2, dxbc, sizeof(dxbc)}, {3, wide, wide_bytes}};
    check(error_of(build(oversize, 3)) == McfxError::texture, "mcfx: a texture wider than 512 is refused");
    const Section indexed[]{{1, reinterpret_cast<const u8*>(manifest), c_string_length(manifest)}, {2, dxbc, sizeof(dxbc)}, {3, palette, palette_bytes}};
    check(error_of(build(indexed, 3)) == McfxError::texture, "mcfx: a palette texture is refused");
    const Section extra[]{{1, reinterpret_cast<const u8*>(kManifest), c_string_length(kManifest)}, {2, dxbc, sizeof(dxbc)}, {3, png, png_bytes}};
    check(error_of(build(extra, 3)) == McfxError::manifest, "mcfx: a texture section the manifest does not declare is refused");
    u32 w = 0, h = 0;
    check(mcfx_png_extent({png, png_bytes}, &w, &h) && w == 64 && h == 32 && !mcfx_png_extent({wide, wide_bytes}, &w, &h) &&
              !mcfx_png_extent({png, 20}, &w, &h),
          "mcfx: the PNG extent reader bounds its input");
}

// ---- Interface 3: the pass graph, TEXTURE2/3 and float data ------------------------------------------------------
// spark-ribbon's graph: a persistent simulation, a grid into a half-size float target, a full-screen composite and
// instanced quads. Shader 0 is DXBC; SHADERS holds 1..5 (vertex, pixel, pixel, vertex, pixel).
constexpr char kGraphManifest[] =
    "{\"interface\":3,\"slug\":\"ribbon\",\"name\":\"Ribbon\",\"version\":\"1.0.0\",\"authors\":[{\"name\":\"A\"}],"
    "\"license\":\"MIT\",\"blend\":\"normal\",\"uses\":[],\"knobs\":[],"
    "\"targets\":[{\"format\":\"rgba32f\",\"size\":[32,32],\"persistent\":true},{\"format\":\"rgba16f\",\"scale\":50}],"
    "\"passes\":[{\"ps\":0,\"target\":0,\"inputs\":[0]},"
    "{\"vs\":1,\"ps\":2,\"grid\":[96,24],\"target\":1,\"blend\":\"add\",\"clear\":true},"
    "{\"ps\":3,\"inputs\":[1]},{\"vs\":4,\"ps\":5,\"quads\":1024,\"inputs\":[0],\"blend\":\"add\"}]}";
constexpr u32 kVertex = 0x10050, kPixel = 0x50;
constexpr u32 kGraphStages[]{kVertex, kPixel, kPixel, kVertex, kPixel, kPixel, kPixel, kPixel, kPixel, kPixel, kPixel, kPixel};
u8 more[sizeof(dxbc) * 12];

// A graph package: `manifest` over a ps_5_0 DXBC and `count` shaders in SHADERS (none: no section), of `stages`.
u32 graph_package(const char* manifest = kGraphManifest, u32 count = 5, const u32* stages = kGraphStages) noexcept {
    for (u32 i = 0; i < count; ++i) {
        make_dxbc(stages[i]);
        copy_bytes(more + i * sizeof(dxbc), dxbc, sizeof(dxbc));
    }
    make_dxbc(kPixel);
    const Section sections[]{{1, reinterpret_cast<const u8*>(manifest), c_string_length(manifest)},
                             {2, dxbc, sizeof(dxbc)},
                             {9, more, count * static_cast<u32>(sizeof(dxbc))}};
    return build(sections, count ? 3 : 2);
}
// The graph manifest with `from` replaced by `to`, as a package; its error.
McfxError graph_error(const char* from, const char* to, u32 count = 5, const u32* stages = kGraphStages) noexcept {
    const char* manifest = replace(kGraphManifest, from, to, edited);
    return manifest ? error_of(graph_package(manifest, count, stages)) : McfxError::invalid_input;
}
bool graph_refuses(const char* from, const char* to) noexcept {
    return graph_error(from, to) == McfxError::manifest;
}

void test_graph(Check check) noexcept {
    const auto& p = workspace.package;
    const auto& g = p.graph;
    check(parses(graph_package()) && p.interface_version == 3 && g.pass_count == 4 && g.target_count == 2 &&
              g.shader_count == 6 && g.vertex_shaders == (1u << 1 | 1u << 4) && p.shaders[5].count == sizeof(dxbc) &&
              g.targets[0].width == 32 && g.targets[0].height == 32 && !g.targets[0].scale && g.targets[0].persistent &&
              g.targets[0].format == EffectTargetFormat::rgba32f && g.targets[1].scale == 50 && !g.targets[1].width &&
              g.targets[1].format == EffectTargetFormat::rgba16f && !g.targets[1].persistent &&
              g.passes[0].target == 0 && g.passes[0].inputs == 1 && g.passes[0].draw == EffectPassDraw::screen &&
              g.passes[1].vertex == 1 && g.passes[1].pixel == 2 && g.passes[1].draw == EffectPassDraw::grid &&
              g.passes[1].count[0] == 96 && g.passes[1].count[1] == 24 && g.passes[1].blend == EffectPassBlend::add &&
              g.passes[1].clear && g.passes[2].target == kEffectGraphOutput && g.passes[2].inputs == 2 &&
              g.passes[2].blend == EffectPassBlend::opaque && g.passes[3].draw == EffectPassDraw::quads &&
              g.passes[3].count[0] == 1024 && g.passes[3].count[1] == 1,
          "mcfx graph: targets, passes, geometry, blends and shader stages parse into the graph");
    check(parses(good_package()) && !p.graph.pass_count && p.graph.shader_count == 1 && p.interface_version == 1,
          "mcfx graph: a package without one is the single pass");
    // Interface: 3, and the graph's members need it.
    check(graph_refuses("\"interface\":3", "\"interface\":2") && graph_refuses("\"interface\":3", "\"interface\":4"),
          "mcfx graph: a graph needs interface 3");
    // Caps: passes, targets, sizes, scales, geometry.
    check(graph_refuses("\"inputs\":[0],\"blend\":\"add\"}]", "\"inputs\":[0],\"blend\":\"add\"},{\"ps\":3},{\"ps\":3},{\"ps\":3}]") &&
              graph_refuses("\"scale\":50}]", "\"scale\":50},{\"format\":\"rgba8\",\"size\":[1,1]},"
                                              "{\"format\":\"rgba8\",\"size\":[1,1]},{\"format\":\"rgba8\",\"size\":[1,1]}]") &&
              graph_refuses("\"size\":[32,32]", "\"size\":[513,32]") && graph_refuses("\"size\":[32,32]", "\"size\":[32,0]") &&
              graph_refuses("\"size\":[32,32]", "\"size\":[32]") && graph_refuses("\"scale\":50", "\"scale\":0") &&
              graph_refuses("\"scale\":50", "\"scale\":101") && graph_refuses("\"scale\":50", "\"scale\":50,\"size\":[1,1]") &&
              graph_refuses(",\"scale\":50", "") && graph_refuses("\"rgba16f\"", "\"r16f\"") &&
              graph_refuses("\"grid\":[96,24]", "\"grid\":[1,24]") && graph_refuses("\"grid\":[96,24]", "\"grid\":[257,24]") &&
              graph_refuses("\"quads\":1024", "\"quads\":0") && graph_refuses("\"quads\":1024", "\"quads\":16385") &&
              graph_refuses("\"quads\":1024", "\"quads\":1024,\"grid\":[2,2]") && graph_refuses("\"quads\":1024,", ""),
          "mcfx graph: at most 6 passes and 4 targets; sizes, scales, formats, grids and quad counts are capped");
    // Memory: fixed targets up to 8 MiB with both planes, scaled ones up to 16 bytes per output pixel.
    check(graph_error("\"size\":[32,32]", "\"size\":[512,512]") == McfxError::none &&
              graph_refuses("\"size\":[32,32]", "\"size\":[512,512]},{\"format\":\"rgba8\",\"size\":[1,1]") &&
              graph_error("\"rgba16f\",\"scale\":50", "\"rgba16f\",\"scale\":100,\"persistent\":true") == McfxError::none &&
              graph_refuses("\"rgba16f\",\"scale\":50", "\"rgba32f\",\"scale\":100,\"persistent\":true"),
          "mcfx graph: target memory is capped, fixed and per output pixel");
    // Wiring: shader indices, stages, targets, inputs, the output, every shader used.
    check(graph_refuses("{\"ps\":3,", "{\"ps\":6,") && graph_refuses("{\"ps\":3,", "{\"ps\":12,") &&
              graph_refuses("{\"ps\":3,", "{") && graph_refuses("\"vs\":1,", "") && graph_refuses("\"vs\":1,", "\"vs\":0,") &&
              graph_refuses("\"target\":1,", "\"target\":2,") && graph_refuses("{\"ps\":3,\"inputs\":[1]}", "{\"ps\":3,\"inputs\":[2]}") &&
              graph_refuses("\"inputs\":[1]", "\"inputs\":[1,1]") &&
              graph_refuses("\"target\":1,", "\"target\":1,\"inputs\":[1],") &&
              graph_refuses("{\"ps\":3,\"inputs\":[1]}", "{\"ps\":3,\"inputs\":[1],\"target\":1}") &&
              graph_refuses("\"blend\":\"add\",\"clear\"", "\"blend\":\"multiply\",\"clear\"") &&
              graph_refuses("\"clear\":true", "\"clear\":true,\"depth\":1") && graph_refuses("\"persistent\":true", "\"planes\":2"),
          "mcfx graph: indices, inputs, own-target reads (only persistent), the output and members are checked");
    check(error_of(graph_package(kGraphManifest, 6)) == McfxError::manifest &&
              error_of(graph_package(kGraphManifest, 4)) == McfxError::manifest &&
              error_of(graph_package(kGraphManifest, 0)) == McfxError::manifest,
          "mcfx graph: every shader is used and every pass's shader exists");
    // Both output passes moved into target 1: nothing draws the output.
    const char* into_target = replace(kGraphManifest, "{\"ps\":3,\"inputs\":[1]}", "{\"ps\":3,\"target\":1}", ambient_manifest);
    const char* no_output = replace(into_target, "\"inputs\":[0],\"blend\":\"add\"}]", "\"inputs\":[0],\"target\":1}]", edited);
    check(no_output && error_of(graph_package(no_output)) == McfxError::manifest, "mcfx graph: a graph must draw the output");
    constexpr u32 swapped[]{kPixel, kPixel, kPixel, kVertex, kPixel};
    check(error_of(graph_package(kGraphManifest, 5, swapped)) == McfxError::bytecode &&
              error_of(graph_package(kGraphManifest, 12)) == McfxError::bytecode,
          "mcfx graph: a shader of the wrong stage, or more than 12 shaders, is refused");
    // SHADERS is a run of whole containers: one that claims more bytes than the section holds is refused.
    graph_package();
    copy_bytes(mutated, more, sizeof(more));
    put32(mutated + 24, 1000);
    const Section overlong[]{{1, reinterpret_cast<const u8*>(kGraphManifest), c_string_length(kGraphManifest)},
                             {2, dxbc, sizeof(dxbc)},
                             {9, mutated, 5 * static_cast<u32>(sizeof(dxbc))}};
    check(error_of(build(overlong, 3)) == McfxError::bytecode, "mcfx graph: SHADERS must split into whole containers");
    // A SHADERS section without passes, and interface 3 without a graph.
    const char* plain = replace(kManifest, "\"interface\":1", "\"interface\":3", edited);
    make_dxbc(kPixel);
    copy_bytes(more, dxbc, sizeof(dxbc));
    const Section stray[]{{1, reinterpret_cast<const u8*>(plain), c_string_length(plain)}, {2, dxbc, sizeof(dxbc)},
                          {9, more, sizeof(dxbc)}};
    check(error_of(build(stray, 3)) == McfxError::manifest && parses(good_package(plain)) && !p.graph.pass_count &&
              p.interface_version == 3,
          "mcfx graph: shaders without passes are refused; interface 3 alone changes nothing");
    // The spark-ribbon sample as maicord-plugin packs it (see cmake/test_registry.cmake): the same graph.
    static u8 sample[kMcfxMaximumBytes];
    DWORD read = 0;
    const HANDLE handle = CreateFileW(MAICORD_GRAPH_PACKAGE, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool loaded = handle != INVALID_HANDLE_VALUE && ReadFile(handle, sample, sizeof(sample), &read, nullptr);
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    const McfxResult built = loaded ? mcfx_parse({sample, read}, &workspace) : McfxResult{McfxError::invalid_input, 0};
    check(built.ok() && p.interface_version == 3 && text_is(p.slug, "spark-ribbon") && g.pass_count == 4 &&
              g.target_count == 2 && g.shader_count == 6 && g.vertex_shaders == (1u << 1 | 1u << 4) &&
              g.passes[1].draw == EffectPassDraw::grid && g.passes[1].count[0] == 96 && g.passes[3].count[0] == 1024 &&
              g.targets[0].persistent && g.targets[1].scale == 50,
          "mcfx graph: the graph sample built by maicord-plugin parses into its graph");
}

// TEXTURE2 and TEXTURE3, and float data textures: interface 3.
void test_textures_three(Check check) noexcept {
    static u8 png[64], data[8 + 4 * 2 * 16], bad[8 + 4 * 2 * 16 - 16];
    const u32 png_bytes = make_png(png, 16, 16, 2);
    copy_bytes(data, "MCFT", 4);
    put16(data + 4, 4);
    put16(data + 6, 2);
    copy_bytes(bad, data, 8);
    static char manifest[2048];
    const char* three = replace(replace(kManifest, "\"interface\":1", "\"interface\":3", edited),
                                "\"uses\":[\"noise\",\"glyphs\"]", "\"uses\":[\"texture0\",\"texture2\",\"texture3\"]", manifest);
    make_dxbc(kPixel);
    const Section sections[]{{1, reinterpret_cast<const u8*>(three), c_string_length(three)}, {2, dxbc, sizeof(dxbc)},
                             {3, data, sizeof(data)}, {7, png, png_bytes}, {8, data, sizeof(data)}};
    u32 width = 0, height = 0;
    check(parses(build(sections, 5)) &&
              workspace.package.uses == (effect_use_texture0 | effect_use_texture2 | effect_use_texture3) &&
              workspace.package.texture[2].count == png_bytes && workspace.package.texture[3].count == sizeof(data) &&
              mcfx_float_texture(workspace.package.texture[0], &width, &height) && width == 4 && height == 2 &&
              !mcfx_float_texture(workspace.package.texture[2], &width, &height),
          "mcfx: four textures, PNG or float data, under interface 3");
    const Section short_data[]{{1, reinterpret_cast<const u8*>(three), c_string_length(three)}, {2, dxbc, sizeof(dxbc)},
                               {3, bad, sizeof(bad)}, {7, png, png_bytes}, {8, data, sizeof(data)}};
    check(error_of(build(short_data, 5)) == McfxError::texture, "mcfx: a float texture must hold exactly its texels");
    put16(bad + 4, 0);
    const Section empty_data[]{{1, reinterpret_cast<const u8*>(three), c_string_length(three)}, {2, dxbc, sizeof(dxbc)},
                               {3, bad, sizeof(bad)}, {7, png, png_bytes}, {8, data, sizeof(data)}};
    check(error_of(build(empty_data, 5)) == McfxError::texture, "mcfx: a float texture needs an extent");
    const char* two = replace(three, "\"interface\":3", "\"interface\":2", edited);
    const Section older[]{{1, reinterpret_cast<const u8*>(two), c_string_length(two)}, {2, dxbc, sizeof(dxbc)},
                          {3, png, png_bytes}, {7, png, png_bytes}, {8, png, png_bytes}};
    const char* one = replace(kManifest, "\"uses\":[\"noise\",\"glyphs\"]", "\"uses\":[\"texture0\"]", manifest);
    const Section floats_one[]{{1, reinterpret_cast<const u8*>(one), c_string_length(one)}, {2, dxbc, sizeof(dxbc)},
                               {3, data, sizeof(data)}};
    check(error_of(build(older, 5)) == McfxError::manifest && error_of(build(floats_one, 3)) == McfxError::manifest,
          "mcfx: TEXTURE2/3 and float data need interface 3");
}

PackageLink link;
bool parsed(const char* text) noexcept { return package_link_parse({text, c_string_length(text)}, &link); }

void test_links(Check check) noexcept {
    check(parsed("maicord://install/effect/aurora") && link.type == PackageLinkType::catalog && link.kind == PackageLinkKind::effect &&
              text_is(link.slug, "aurora") && parsed("MAICORD://install/effect/a") && link.type == PackageLinkType::catalog,
          "link: a catalog link names a kind and a slug");
    check(parsed("maicord://install/theme/fjord-2/") && link.kind == PackageLinkKind::theme && text_is(link.slug, "fjord-2") &&
              parsed("maicord://install/plugin/translate") && link.kind == PackageLinkKind::plugin,
          "link: theme and plugin kinds, with an optional trailing slash");
    check(!parsed("maicord://install/effect/") && !parsed("maicord://install/effect") && !parsed("maicord://install/effect/Aurora") &&
              !parsed("maicord://install/effect/-a") && !parsed("maicord://install/effect/a/b") &&
              !parsed("maicord://install/gizmo/a") && !parsed("maicord://open") && !parsed("maicord://install/effect/a?x=1") &&
              !parsed("maicord://install/effect/a%20b") && !parsed("maicord:/install/effect/a") && !parsed("maicord//install/effect/a"),
          "link: the grammar is closed; kinds, slugs and extra parts outside it are dropped");
    check(parsed("maicord://install?url=https://example.org/a.mcfx") && link.type == PackageLinkType::url &&
              text_is(link.url, "https://example.org/a.mcfx") &&
              parsed("maicord://install?url=https%3A%2F%2Fexample.org%2Fa.mcfx%3Fv%3D1") &&
              text_is(link.url, "https://example.org/a.mcfx?v=1") && parsed("https://example.org/x.maicord-theme.json") &&
              link.type == PackageLinkType::url,
          "link: a package URL, raw, percent-encoded or direct");
    check(!parsed("maicord://install?url=http://example.org/a.mcfx") && !parsed("maicord://install?url=ftp://example.org/a") &&
              !parsed("maicord://install?url=https://example.org/a&b=1") && !parsed("maicord://install?url=") &&
              !parsed("maicord://install?url=https://example.org/a#x") && !parsed("maicord://install?url=https%3A%2F%2Fexample.org%2F%zz") &&
              !parsed("maicord://install?uri=https://example.org/a") && !parsed("maicord://install?url=https://example.org/a?x=1&url=2"),
          "link: only one https url parameter is accepted, escapes must be valid, extra parameters are refused");
    check(!parsed("http://example.org/a.mcfx") && !parsed("https://user:pw@example.org/a") && !parsed("https://example.org:8443/a") &&
              !parsed("https://127.0.0.1/a") && !parsed("https://localhost/a") && !parsed("https:///a") && !parsed("https://exa mple.org/a") &&
              !parsed("https://example.org/a b") && !parsed("https://example.org/\x01") && !parsed("https://example.org/\xc3\xa9") &&
              !parsed("") && !parsed("file:///c:/x.mcfx") && !parsed("javascript:alert(1)"),
          "link: hosts must be plain DNS names; ports, userinfo, addresses, spaces, controls and non-ASCII are refused");
    static char long_url[kPackageUrlMaximumBytes + 64];
    copy_bytes(long_url, "https://example.org/", 20);
    for (u32 i = 20; i < sizeof(long_url) - 1; ++i)
        long_url[i] = 'a';
    long_url[kPackageUrlMaximumBytes] = 0;
    const bool at_limit = parsed(long_url);
    long_url[kPackageUrlMaximumBytes + 1] = 0;
    long_url[kPackageUrlMaximumBytes] = 'a';
    check(at_limit && !parsed(long_url), "link: a URL of 2,048 bytes is accepted and one byte more is not");
    const wchar_t wide_ok[] = L"maicord://install/effect/aurora", wide_bad[] = L"maicord://install/effect/au\u00e9";
    check(package_link_parse_wide({wide_ok, sizeof(wide_ok) / 2 - 1}, &link) && link.type == PackageLinkType::catalog &&
              !package_link_parse_wide({wide_bad, sizeof(wide_bad) / 2 - 1}, &link) && !package_link_parse_wide({nullptr, 0}, &link),
          "link: the wide form takes ASCII only");
    UpdateLocation location{};
    parsed("maicord://install?url=https://Example.org/dir/a.mcfx?v=2");
    const bool located = package_link_location(link, &location) && location.host == UpdateHost::provider &&
                         location.origin_bytes == 11 && text_is(location.origin, "example.org") &&
                         text_is(location.target, "/dir/a.mcfx?v=2");
    parsed("maicord://install/effect/aurora");
    check(located && !package_link_location(link, &location),
          "link: a url link fetches from its own host only; a catalog link has no location");
    check(package_slug_valid(utf8_literal("a-b9")) && !package_slug_valid(utf8_literal("")) && !package_slug_valid(utf8_literal("A")) &&
              !package_slug_valid(utf8_literal("a_b")) && !package_slug_valid(utf8_literal("-a")),
          "link: the slug grammar matches the catalog's");
}

// A private folder under %TEMP% stands in for %LOCALAPPDATA%\MaiCord.
bool exists(const wchar_t* path) noexcept { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

void test_install(Check check) noexcept {
    wchar_t root[kPackageRootUnits];
    const u32 length = GetTempPathW(kPackageRootUnits - 64, root);
    wchar_t* tail = root + length;
    const wchar_t prefix[] = L"maicord-packages-";
    copy_bytes(tail, prefix, sizeof(prefix));
    u32 at = length + 17;
    const u32 id = GetCurrentProcessId() ^ static_cast<u32>(GetTickCount64());
    for (u32 i = 0; i < 8; ++i)
        root[at++] = L"0123456789abcdef"[(id >> (28 - i * 4)) & 15];
    root[at] = 0;
    u8 digest[32]{};
    static const u8 abc[3]{'a', 'b', 'c'};
    check(package_sha256({abc, 3}, digest) && digest[0] == 0xba && digest[1] == 0x78 && digest[2] == 0x16 && digest[31] == 0xad,
          "install: SHA-256 of a known input");
    const u32 bytes = good_package();
    const Utf8View slug = utf8_literal("rain-drops");
    wchar_t path[512], other[512];
    u32 units = 0, other_units = 0;
    u8 first[32]{};
    check(package_install(root, PackageLinkKind::effect, slug, {file, bytes}, first) &&
              package_file_path(root, PackageLinkKind::effect, slug, first, {path, 512}, &units) && exists(path) &&
              path[units - 5] == L'.' && path[units - 22] == L'-',
          "install: an effect lands in effects\\<slug>-<sha16>.mcfx");
    // An update: a different file for the slug; the older one goes only when it is pruned.
    const char* newer = edit("\"version\":\"1.2.0\"", "\"version\":\"1.3.0\"");
    const u32 newer_bytes = good_package(newer);
    u8 second[32]{};
    check(package_install(root, PackageLinkKind::effect, slug, {file, newer_bytes}, second) &&
              package_file_path(root, PackageLinkKind::effect, slug, second, {other, 512}, &other_units) && exists(other) &&
              exists(path),
          "install: an update writes beside the earlier version");
    check(package_prune(root, slug, second) && exists(other) && !exists(path),
          "install: pruning keeps the named file and removes the other versions of the slug");
    // A longer slug that shares the prefix is another effect and keeps its files.
    u8 sibling_digest[32]{};
    const Utf8View sibling = utf8_literal("rain-drops-2");
    check(package_install(root, PackageLinkKind::effect, sibling, {file, newer_bytes}, sibling_digest) &&
              package_prune(root, slug, second) && package_file_path(root, PackageLinkKind::effect, sibling, sibling_digest, {path, 512}, &units) &&
              exists(path),
          "install: pruning one slug never touches a slug that starts with it");
    // The installed file is a package the loader accepts.
    static u8 again[kMcfxMaximumBytes];
    HANDLE handle = CreateFileW(other, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD read = 0;
    const bool readable = handle != INVALID_HANDLE_VALUE && ReadFile(handle, again, sizeof(again), &read, nullptr) && read == newer_bytes;
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    check(readable && bytes_equal(again, file, newer_bytes) && mcfx_parse({again, read}, &workspace).ok() &&
              text_is(workspace.package.version, "1.3.0"),
          "install: the installed bytes are the validated package");
    // A theme file.
    static const char theme[] = "{\"version\":\"2.0\",\"name\":\"Night\"}";
    u8 theme_digest[32]{};
    wchar_t theme_path[512];
    u32 theme_units = 0;
    check(package_install(root, PackageLinkKind::theme, utf8_literal("night-1a2b3c4d"), {reinterpret_cast<const u8*>(theme), sizeof(theme) - 1}, theme_digest) &&
              package_file_path(root, PackageLinkKind::theme, utf8_literal("night-1a2b3c4d"), nullptr, {theme_path, 512}, &theme_units) &&
              exists(theme_path) && theme_path[theme_units - 1] == L'n',
          "install: a theme lands in themes\\<slug>.maicord-theme.json");
    check(!package_file_path(root, PackageLinkKind::theme, utf8_literal("..\\x"), nullptr, {theme_path, 512}, &theme_units) &&
              !package_file_path(root, PackageLinkKind::plugin, slug, first, {theme_path, 512}, &theme_units) &&
              !package_install(root, PackageLinkKind::effect, utf8_literal("A/B"), {file, bytes}, first),
          "install: a slug that is not valid never builds a path");
    check(package_uninstall(root, PackageLinkKind::effect, slug) && !exists(other) && package_uninstall(root, PackageLinkKind::effect, slug) &&
              package_uninstall(root, PackageLinkKind::theme, utf8_literal("night-1a2b3c4d")) && !exists(theme_path) &&
              package_uninstall(root, PackageLinkKind::theme, utf8_literal("night-1a2b3c4d")),
          "install: removal deletes the slug's files and is idempotent");
    package_uninstall(root, PackageLinkKind::effect, sibling);
    // Clean the private folders.
    wchar_t folder[512];
    const wchar_t* const leaves[]{L"\\effects", L"\\themes", L""};
    for (const wchar_t* leaf : leaves) {
        u32 n = 0;
        folder[0] = 0;
        wide_append(folder, 512, &n, root);
        wide_append(folder, 512, &n, leaf);
        RemoveDirectoryW(folder);
    }
    check(!exists(root), "install: the test folder is removed");
}

} // namespace

void test_packages(Check check) noexcept {
    test_parse(check);
    test_container(check);
    test_bytecode(check);
    test_manifest(check);
    test_textures(check);
    test_graph(check);
    test_textures_three(check);
    test_mutation(check);
    test_links(check);
    test_install(check);
}

namespace packages_entry {
u32 failures;
void check(bool passed, const char* message) noexcept { test_check(&failures, passed, message); }
} // namespace packages_entry
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_packages_tests_entry() noexcept {
    maicord::test_packages(maicord::packages_entry::check);
    ExitProcess(maicord::packages_entry::failures ? 1 : 0);
}
