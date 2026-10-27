#include "maicord/text/sfnt.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
bool truetype_outline_tests() noexcept;
bool cff_outline_tests() noexcept;
bool color_font_tests() noexcept;
bool variation_store_tests() noexcept;
bool font_axes_tests() noexcept;
bool tt_bold_points_tests() noexcept;
namespace {

u8 bytes[2048];
constexpr u32 cmap_offset = 320;

void put16(u8* p, u16 value) noexcept { p[0] = static_cast<u8>(value >> 8u); p[1] = static_cast<u8>(value); }
void put24(u8* p, u32 value) noexcept { p[0] = static_cast<u8>(value >> 16u); put16(p + 1, static_cast<u16>(value)); }
void put32(u8* p, u32 value) noexcept { byte_store_be32(p, value); }

void table(u32 index, u32 tag, u32 at, u32 length) noexcept {
    u8* p = bytes + 12 + index * 16;
    put32(p, tag); put32(p + 8, at); put32(p + 12, length);
}

u8* fixture(u16 format, u32 size, bool uvs = false) noexcept {
    clear_bytes(bytes, sizeof(bytes));
    put32(bytes, 0x10000); put16(bytes + 4, 5);
    table(0, sfnt_tag('c','m','a','p'), cmap_offset, (uvs ? 20u : 12u) + size);
    table(1, sfnt_tag('h','e','a','d'), 128, 54);
    table(2, sfnt_tag('h','h','e','a'), 184, 36);
    table(3, sfnt_tag('h','m','t','x'), 224, 68);
    table(4, sfnt_tag('m','a','x','p'), 96, 32);
    put32(bytes + 96, 0x10000); put16(bytes + 100, 32);
    put32(bytes + 128, 0x10000); put32(bytes + 140, 0x5f0f3cf5); put16(bytes + 146, 1000);
    put32(bytes + 184, 0x10000); put16(bytes + 188, 800); put16(bytes + 190, static_cast<u16>(-200));
    put16(bytes + 192, 100); put16(bytes + 218, 2);
    put16(bytes + 224, 400); put16(bytes + 226, static_cast<u16>(-10));
    put16(bytes + 228, 500); put16(bytes + 230, 20);
    put16(bytes + 232, static_cast<u16>(-30)); put16(bytes + 290, 31);
    u8* cmap = bytes + cmap_offset;
    put16(cmap + 2, uvs ? 2 : 1);
    put16(cmap + 4, 3); put16(cmap + 6, format >= 10 ? 10 : 1);
    put32(cmap + 8, uvs ? 20 : 12);
    u8* p = cmap + (uvs ? 20 : 12);
    put16(p, format);
    if (format >= 10) put32(p + 4, size);
    else put16(p + 2, static_cast<u16>(size));
    return p;
}

SfntStatus open(SfntCmap* cmap) noexcept {
    SfntFace face{};
    const auto status = sfnt_open({bytes, sizeof(bytes)}, 0, &face);
    return status == SfntStatus::ok ? sfnt_cmap_open(face, cmap) : status;
}

bool mapping(const SfntCmap& cmap, u32 scalar, u16 expected, u32 selector = 0,
               SfntStatus status = SfntStatus::ok) noexcept {
    u16 glyph = 0xabcd;
    return sfnt_cmap_glyph(cmap, scalar, selector, &glyph) == status && glyph == expected;
}

bool nominal_cases() noexcept {
    SfntCmap cmap{};
    u8* p = fixture(0, 262);
    p[6 + 'A'] = 17;
    if (open(&cmap) != SfntStatus::ok || !mapping(cmap, 'A', 17) || !mapping(cmap, 0x100, 0)) return false;
    p = fixture(6, 16); put16(p + 6, 0x400); put16(p + 8, 3);
    put16(p + 10, 9); put16(p + 12, 0); put16(p + 14, 12);
    if (open(&cmap) != SfntStatus::ok || !mapping(cmap, 0x3ff, 0) || !mapping(cmap, 0x400, 9) ||
        !mapping(cmap, 0x401, 0) || !mapping(cmap, 0x402, 12) || !mapping(cmap, 0x403, 0)) return false;
    p = fixture(10, 26); put32(p + 12, 0x10000); put32(p + 16, 3);
    put16(p + 20, 8); put16(p + 22, 9); put16(p + 24, 10);
    if (open(&cmap) != SfntStatus::ok || !mapping(cmap, 0xffff, 0) || !mapping(cmap, 0x10002, 10) ||
        !mapping(cmap, 0x10003, 0)) return false;
    for (u16 format = 12; format <= 13; ++format) {
        p = fixture(format, 40); put32(p + 12, 2);
        put32(p + 16, 'A'); put32(p + 20, 'C'); put32(p + 24, 4);
        put32(p + 28, 0x1f600); put32(p + 32, 0x1f602); put32(p + 36, 20);
        if (open(&cmap) != SfntStatus::ok || !mapping(cmap, 'A', 4) ||
            !mapping(cmap, 'C', format == 12 ? 6 : 4) || !mapping(cmap, 'D', 0) ||
            !mapping(cmap, 0x1f601, format == 12 ? 21 : 20) || !mapping(cmap, 0x10ffff, 0)) return false;
    }
    return mapping(cmap, 0x110000, 0, 0, SfntStatus::invalid_input) &&
           mapping(cmap, 0xd800, 0, 0, SfntStatus::invalid_input);
}

bool format4_cases() noexcept {
    // A..C uses idDelta; a..c uses glyphIdArray and a nonzero idDelta.
    // The middle glyphIdArray entry must remain zero, without applying delta.
    u8* p = fixture(4, 46);
    put16(p + 6, 6);
    put16(p + 14, 'C'); put16(p + 16, 'c'); put16(p + 18, 0xffff);
    put16(p + 22, 'A'); put16(p + 24, 'a'); put16(p + 26, 0xffff);
    put16(p + 28, static_cast<u16>(3 - 'A')); put16(p + 30, 2); put16(p + 32, 1);
    put16(p + 36, 4); put16(p + 40, 10); put16(p + 42, 0); put16(p + 44, 20);
    SfntCmap cmap{};
    if (open(&cmap) != SfntStatus::ok || !mapping(cmap, 'A', 3) || !mapping(cmap, 'C', 5) ||
        !mapping(cmap, 'D', 0) || !mapping(cmap, 'a', 12) || !mapping(cmap, 'b', 0) ||
        !mapping(cmap, 'c', 22) || !mapping(cmap, 0xffff, 0) || !mapping(cmap, 0x10000, 0)) return false;
    put16(p + 36, 0xfffe);
    if (open(&cmap) != SfntStatus::truncated || cmap.base.data) return false;
    put16(p + 36, 3);
    if (open(&cmap) != SfntStatus::malformed) return false;
    put16(p + 36, 4); put16(p + 24, 'B');
    return open(&cmap) == SfntStatus::malformed;
}

bool variation_cases() noexcept {
    u8* p = fixture(12, 28, true);
    put32(p + 12, 1); put32(p + 16, 'A'); put32(p + 20, 'C'); put32(p + 24, 4);
    u8* cmap_table = bytes + cmap_offset;
    put16(cmap_table + 12, 0); put16(cmap_table + 14, 5); put32(cmap_table + 16, 48);
    u8* v = cmap_table + 48;
    put16(v, 14); put32(v + 2, 38); put32(v + 6, 1);
    put24(v + 10, 0xfe0f); put32(v + 13, 21); put32(v + 17, 29);
    put32(v + 21, 1); put24(v + 25, 'A'); v[28] = 1;
    put32(v + 29, 1); put24(v + 33, 'C'); put16(v + 36, 19);
    put32(bytes + 24, 86);
    SfntCmap cmap{};
    if (open(&cmap) != SfntStatus::ok || !mapping(cmap, 'A', 4, 0xfe0f) ||
        !mapping(cmap, 'B', 5, 0xfe0f) || !mapping(cmap, 'C', 19, 0xfe0f) ||
        !mapping(cmap, 'D', 0, 0xfe0f, SfntStatus::unsupported_sequence) ||
        !mapping(cmap, 'A', 0, 0xfe0e, SfntStatus::unsupported_sequence) ||
        !mapping(cmap, 'A', 0, 'x', SfntStatus::invalid_input)) return false;
    put32(v + 17, 0xfffffff0);
    if (open(&cmap) != SfntStatus::truncated) return false;
    put32(v + 17, 29); put16(v + 36, 32);
    return open(&cmap) == SfntStatus::malformed;
}

bool selected_cmap_cases() noexcept {
    for (u32 reverse = 0; reverse < 2; ++reverse) {
        u8* lower = fixture(4, 24, true);
        put16(lower + 6, 2); put16(lower + 14, 0xffff);
        put16(lower + 18, 0xffff); put16(lower + 20, 1);
        auto* directory = bytes + cmap_offset;
        u8* selected = lower + 24;
        put16(selected, 12); put32(selected + 4, 28); put32(selected + 12, 1);
        put32(selected + 16, 'A'); put32(selected + 20, 'C'); put32(selected + 24, 4);
        put16(directory + 4, 0); put16(directory + 6, 3); put32(directory + 8, 20);
        put16(directory + 12, 3); put16(directory + 14, 10); put32(directory + 16, 44);
        put32(bytes + 24, 72);
        if (reverse) {
            u8 record[8]; copy_bytes(record, directory + 4, 8);
            copy_bytes(directory + 4, directory + 12, 8); copy_bytes(directory + 12, record, 8);
        }
        SfntCmap cmap{};
        if (open(&cmap) != SfntStatus::ok || cmap.base.data != selected || !mapping(cmap, 'C', 6)) return false;
        put16(lower + 6, 0);
        if (open(&cmap) != SfntStatus::ok || !mapping(cmap, 'A', 4)) return false;
        put32(selected + 24, 32);
        if (open(&cmap) != SfntStatus::malformed || cmap.base.data) return false;
        put32(selected + 24, 4);
        put16(directory + 4, 0); put16(directory + 6, 4); put32(directory + 8, 44);
        put16(directory + 12, 3); put16(directory + 14, 10); put32(directory + 16, 44);
        if (open(&cmap) != SfntStatus::ok || cmap.base.data != selected || !mapping(cmap, 'B', 5)) return false;
    }
    return true;
}

bool directory_metrics_cases() noexcept {
    fixture(0, 262);
    SfntFace face{};
    if (sfnt_open({bytes, sizeof(bytes)}, 0, &face) != SfntStatus::ok) return false;
    ConstSlice<u8> table_view{};
    if (sfnt_table(face, sfnt_tag('n','o','p','e'), &table_view) != SfntStatus::missing_table ||
        table_view.data) return false;
    SfntHorizontalMetrics metrics{};
    if (sfnt_horizontal_metrics_open(face, &metrics) != SfntStatus::ok ||
        metrics.glyph_count != 32 || metrics.units_per_em != 1000 || metrics.ascender != 800 ||
        metrics.descender != -200 || metrics.line_gap != 100) return false;
    SfntGlyphMetrics m{};
    if (sfnt_horizontal_metrics(metrics, 0, &m) != SfntStatus::ok || m.advance != 400 || m.bearing != -10 ||
        sfnt_horizontal_metrics(metrics, 1, &m) != SfntStatus::ok || m.advance != 500 || m.bearing != 20 ||
        sfnt_horizontal_metrics(metrics, 2, &m) != SfntStatus::ok || m.advance != 500 || m.bearing != -30 ||
        sfnt_horizontal_metrics(metrics, 31, &m) != SfntStatus::ok || m.advance != 500 || m.bearing != 31 ||
        sfnt_horizontal_metrics(metrics, 32, &m) != SfntStatus::glyph_out_of_range) return false;
    put16(bytes + 218, 33);
    if (sfnt_horizontal_metrics_open(face, &metrics) != SfntStatus::malformed || metrics.table.data) return false;
    put16(bytes + 218, 2); put32(bytes + 24, 0xfffffff0u);
    if (sfnt_open({bytes, sizeof(bytes)}, 0, &face) != SfntStatus::truncated || face.file.data) return false;
    fixture(0, 262); put32(bytes + 28, sfnt_tag('c','m','a','p'));
    if (sfnt_open({bytes, sizeof(bytes)}, 0, &face) != SfntStatus::malformed) return false;
    fixture(0, 262);
    return sfnt_open({bytes, sizeof(bytes)}, 1, &face) == SfntStatus::face_out_of_range &&
           sfnt_open({nullptr, 100}, 0, &face) == SfntStatus::invalid_input;
}

bool collection_cases() noexcept {
    fixture(0, 262);
    // Move the directory only: TTC table offsets remain absolute. Both faces
    // intentionally share the same tables. This catches face-relative offsets.
    copy_bytes(bytes + 1000, bytes, 92);
    put32(bytes, sfnt_tag('t','t','c','f')); put32(bytes + 4, 0x20000); put32(bytes + 8, 2);
    put32(bytes + 12, 1000); put32(bytes + 16, 1000);
    put32(bytes + 20, 0); put32(bytes + 24, 0); put32(bytes + 28, 0);
    SfntFace face{};
    ConstSlice<u8> cmap{};
    if (sfnt_open({bytes, sizeof(bytes)}, 1, &face) != SfntStatus::ok || face.face_count != 2 ||
        face.face_index != 1 || sfnt_table(face, sfnt_tag('c','m','a','p'), &cmap) != SfntStatus::ok ||
        cmap.data != bytes + cmap_offset) return false;
    if (sfnt_open({bytes, sizeof(bytes)}, 2, &face) != SfntStatus::face_out_of_range) return false;
    put32(bytes + 8, 0xffffffffu);
    return sfnt_open({bytes, sizeof(bytes)}, 0, &face) == SfntStatus::truncated;
}

bool truncation_and_guard_cases() noexcept {
    fixture(12, 28);
    // Every truncated prefix ends immediately before an inaccessible page.
    // Valid directories can still reference a semantically truncated cmap.
    u8* memory = static_cast<u8*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE, PAGE_NOACCESS));
    if (!memory) return false;
    if (!VirtualAlloc(memory, 4096, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(memory, 0, MEM_RELEASE); return false;
    }
    bool ok = true;
    for (u32 size = 0; size <= 400; ++size) {
        u8* data = memory + 4096 - size;
        copy_bytes(data, bytes, size);
        SfntFace face{};
        if (sfnt_open({data, size}, 0, &face) == SfntStatus::ok) {
            SfntCmap cmap{};
            if (sfnt_cmap_open(face, &cmap) == SfntStatus::ok) {
                u16 glyph = 0;
                ok = ok && sfnt_cmap_glyph(cmap, 'A', 0, &glyph) == SfntStatus::ok;
            }
            SfntHorizontalMetrics metrics{};
            sfnt_horizontal_metrics_open(face, &metrics);
        }
    }
    VirtualFree(memory, 0, MEM_RELEASE);
    return ok;
}

bool packed_cmap_guard_cases() noexcept {
    // Relocate complete valid fonts to every byte alignment, including an
    // exact guard-page boundary. Expected mappings come from the fixture's
    // arithmetic definition, independently of the cmap search implementation.
    auto* memory = static_cast<u8*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE, PAGE_NOACCESS));
    if (!memory) return false;
    if (!VirtualAlloc(memory, 4096, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(memory, 0, MEM_RELEASE); return false;
    }
    bool okay = true;
    const u16 formats[]{4, 12, 13};
    for (u16 format : formats) for (u32 groups = 0; groups <= 64; ++groups) {
        const u32 count = groups + (format == 4 ? 1u : 0u);
        const u32 size = 16 + count * (format == 4 ? 8u : 12u);
        auto* p = fixture(format, size);
        if (format == 4) {
            put16(p + 6, static_cast<u16>(count * 2));
            for (u32 i = 0; i < count; ++i) {
                const u32 first = i == groups ? 0xffffu : 0x400u + i * 4;
                const u32 last = first + (i == groups ? 0u : 1u);
                put16(p + 14 + i * 2, static_cast<u16>(last));
                put16(p + 16 + count * 2 + i * 2, static_cast<u16>(first));
                put16(p + 16 + count * 4 + i * 2,
                    static_cast<u16>(i == groups ? 1u : 1u + i % 24 - first));
            }
        } else {
            put32(p + 12, count);
            for (u32 i = 0; i < count; ++i) {
                put32(p + 16 + i * 12, 0x400 + i * 4);
                put32(p + 20 + i * 12, 0x401 + i * 4);
                put32(p + 24 + i * 12, 1 + i % 24);
            }
        }
        const u32 length = cmap_offset + 12 + size;
        for (u32 gap = 0; gap < 16; ++gap) {
            auto* data = memory + 4096 - length - gap;
            copy_bytes(data, bytes, length);
            SfntFace face{}; SfntCmap cmap{};
            if (sfnt_open({data, length}, 0, &face) != SfntStatus::ok ||
                sfnt_cmap_open(face, &cmap) != SfntStatus::ok) { okay = false; break; }
            for (u32 cp = 0; cp < groups * 4 + 2; ++cp) {
                const u16 expected = cp / 4 < groups && cp % 4 < 2 ?
                    static_cast<u16>(1 + (cp / 4) % 24 + (format == 13 ? 0 : cp % 4)) : 0;
                if (!mapping(cmap, 0x400 + cp, expected)) { okay = false; break; }
            }
            if (!mapping(cmap, 0xffff, 0) || !mapping(cmap, 0x10ffff, 0)) okay = false;
        }
    }
    VirtualFree(memory, 0, MEM_RELEASE);
    return okay;
}

bool group_range_boundaries() noexcept {
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,8192,MEM_RESERVE,PAGE_NOACCESS));
    if(!memory)return false;
    struct Release {void* p;~Release() noexcept {VirtualFree(p,0,MEM_RELEASE);}} release{memory};
    if(!VirtualAlloc(memory,4096,MEM_COMMIT,PAGE_READWRITE))return false;
    constexpr u32 values[]{0,1,2,16,31,32,65534,65535,0x10ffff,0x110000,0x7fffffff,0x80000000,0xffffffff};
    constexpr u16 glyph_counts[]{1,32,65535};
    constexpr u32 counts[]{2,4,5};
    for(u16 format=12;format<=13;++format)for(u16 glyph_count:glyph_counts)
    for(u32 count:counts)for(u32 changed=0;changed<count;++changed)for(u32 first:values)for(u32 last:values)for(u32 glyph:values){
        u32 groups[5][3]{{0,0,0},{16,16,0},{32,32,0},{48,48,0},{64,64,0}};
        groups[changed][0]=first;groups[changed][1]=last;groups[changed][2]=glyph;
        const u32 size=16+count*12,length=cmap_offset+12+size;auto* input=memory+4096-length;
        auto* p=fixture(format,size);put16(bytes+100,glyph_count);put32(p+12,count);
        bool valid=true;
        for(u32 i=0;i<count;++i){const auto& g=groups[i];for(u32 j=0;j<3;++j)put32(p+16+i*12+j*4,g[j]);
            valid=valid&&g[0]<=g[1]&&g[1]<=0x10ffffu&&(!i||g[0]>groups[i-1][1])&&g[2]<glyph_count;
            if(valid&&format==12)valid=static_cast<u64>(g[2])+g[1]-g[0]<glyph_count;
        }
        copy_bytes(input,bytes,length);SfntFace face{};SfntCmap cmap{};
        if(sfnt_open({input,length},0,&face)!=SfntStatus::ok)return false;
        if(sfnt_cmap_open(face,&cmap)!=(valid?SfntStatus::ok:SfntStatus::malformed)||static_cast<bool>(cmap.base.data)!=valid)return false;
    }
    return true;
}

bool malformed_cmap_cases() noexcept {
    SfntCmap cmap{};
    u8* p = fixture(12, 28);
    put32(p + 12, 0xffffffffu);
    if (open(&cmap) != SfntStatus::truncated || cmap.base.data) return false;
    put32(p + 12, 1); put32(p + 16, 'A'); put32(p + 20, 'C'); put32(p + 24, 31);
    if (open(&cmap) != SfntStatus::malformed) return false;
    put32(p + 24, 4); put32(p + 20, 'A' - 1);
    if (open(&cmap) != SfntStatus::malformed) return false;
    p = fixture(10, 20); put32(p + 12, 0x10ffff); put32(p + 16, 2);
    if (open(&cmap) != SfntStatus::malformed) return false;
    fixture(0, 262); put32(bytes + cmap_offset + 8, 0xfffffff0u);
    if (open(&cmap) != SfntStatus::truncated) return false;
    fixture(0, 262); put16(bytes + cmap_offset + 4, 1);
    if (open(&cmap) != SfntStatus::unsupported) return false;
    p = fixture(6, 12); put16(p + 6, 'A'); put16(p + 8, 1); put16(p + 10, 32);
    return open(&cmap) == SfntStatus::ok &&
           mapping(cmap, 'A', 0, 0, SfntStatus::glyph_out_of_range);
}

bool mutation_guard_cases() noexcept {
    // Deterministic structural mutations with a guard immediately after the
    // declared input, including mutated offsets, counts, lengths and formats.
    u8* memory = static_cast<u8*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE, PAGE_NOACCESS));
    if (!memory) return false;
    if (!VirtualAlloc(memory, 4096, MEM_COMMIT, PAGE_READWRITE)) {
        VirtualFree(memory, 0, MEM_RELEASE); return false;
    }
    u32 random = 0x719a8831;
    for (u32 iteration = 0; iteration < 20000; ++iteration) {
        u8* p = fixture(12, 28);
        put32(p + 12, 1); put32(p + 16, 'A'); put32(p + 20, 'Z'); put32(p + 24, 1);
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        const u32 size = 360u - (iteration % 7u == 0 ? random % 360u : 0u);
        u8* input = memory + 4096 - size;
        copy_bytes(input, bytes, size);
        if (size) input[(random >> 8) % size] ^= static_cast<u8>(random | 1u);
        SfntFace face{};
        if (sfnt_open({input, size}, 0, &face) != SfntStatus::ok) continue;
        SfntCmap cmap{};
        if (sfnt_cmap_open(face, &cmap) == SfntStatus::ok) {
            u16 glyph = 0;
            sfnt_cmap_glyph(cmap, random % 0x110000u, 0, &glyph);
            sfnt_cmap_glyph(cmap, 'A', 0xfe0f, &glyph);
        }
        SfntHorizontalMetrics metrics{};
        if (sfnt_horizontal_metrics_open(face, &metrics) == SfntStatus::ok) {
            SfntGlyphMetrics glyph{};
            sfnt_horizontal_metrics(metrics, static_cast<u16>(random), &glyph);
        }
    }
    VirtualFree(memory, 0, MEM_RELEASE);
    return true;
}

int run() noexcept {
    if (!group_range_boundaries()) return 20;
    if (!nominal_cases()) return 1;
    if (!format4_cases()) return 2;
    if (!variation_cases()) return 3;
    if (!selected_cmap_cases()) return 19;
    if (!directory_metrics_cases()) return 4;
    if (!collection_cases()) return 5;
    if (!truncation_and_guard_cases()) return 6;
    if (!malformed_cmap_cases()) return 7;
    if (!mutation_guard_cases()) return 8;
    if (!truetype_outline_tests()) return 9;
    if (!cff_outline_tests()) return 10;
    if (!color_font_tests()) return 11;
    if (!variation_store_tests()) return 12;
    if (!font_axes_tests()) return 13;
    if (!tt_bold_points_tests()) return 17;
    if (!packed_cmap_guard_cases()) return 18;
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_font_tests_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
