#include "maicord/render/effects/colr_program.hpp"
#include "maicord/render/effects/lottie_program.hpp"
#include "maicord/os/file.hpp"
#include "../../gpu/capture_io.hpp"
#include "lottie_geometry_raster.hpp"

// The Lottie compiler's output on a corpus of documents against what Direct2D's geometry made of the same files.
//
// Every document of tests/render/effects/corpus (made by tools/generate_lottie_corpus.py: the kinds of artwork the
// stickers in use have, drawn from seeded curves) is compiled at a few frames. Each compiled program holds one mask
// per painted shape: the outline as line segments in the document's unit square. A mask is digested as its area,
// outline length and bounds and its coverage on a 32 x 32 grid (rasterized on a finer one), and the digests of a frame
// are compared with the ones in tests/render/effects/golden/lottie_program.bin, within what the compiler's flattening
// tolerance allows. Windows compiles with Direct2D (so it checks the file stays current), Linux with the in-house
// geometry (src/render/effects/vector_geometry.cpp): there the file is what Direct2D produced.
//
// To renew it on Windows, set MAICORD_LOTTIE_PROGRAM_CAPTURE=<file> and copy the file to the golden directory.
// To compare a directory of real files without shipping them, list their paths one per line in a file and set
// MAICORD_LOTTIE_CORPUS_LIST=<that file>; with MAICORD_LOTTIE_CORPUS_CAPTURE=<file> a Windows run writes their digests
// and a Linux run with MAICORD_LOTTIE_CORPUS_COMPARE=<that file> compares. MAICORD_LOTTIE_REPORT=1 prints every
// frame's worst differences.
namespace maicord {
namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
alignas(16) constexpr u8 kBlobs[] = {
#embed "corpus/blobs.json"
};
alignas(16) constexpr u8 kMerges[] = {
#embed "corpus/merges.json"
};
alignas(16) constexpr u8 kMattes[] = {
#embed "corpus/mattes.json"
};
alignas(16) constexpr u8 kMasks[] = {
#embed "corpus/masks.json"
};
alignas(16) constexpr u8 kStrokes[] = {
#embed "corpus/strokes.json"
};
alignas(16) constexpr u8 kNested[] = {
#embed "corpus/nested.json"
};
alignas(16) constexpr u8 kCharacter[] = {
#embed "corpus/character.json"
};
alignas(16) constexpr u8 kDense[] = {
#embed "corpus/dense.json"
};
alignas(16) constexpr u8 kFixture[] = {
#embed "golden/lottie_program.bin" if_empty(0)
};
#pragma clang diagnostic pop

struct Document {
    const char* name;
    const u8* data;
    u32 bytes;
};
constexpr Document kDocuments[] = {
    {"blobs", kBlobs, sizeof(kBlobs)},         {"merges", kMerges, sizeof(kMerges)},
    {"mattes", kMattes, sizeof(kMattes)},      {"masks", kMasks, sizeof(kMasks)},
    {"strokes", kStrokes, sizeof(kStrokes)},   {"nested", kNested, sizeof(kNested)},
    {"character", kCharacter, sizeof(kCharacter)}, {"dense", kDense, sizeof(kDense)}};
constexpr u32 kDocumentCount = sizeof(kDocuments) / sizeof(kDocuments[0]);

constexpr u32 kRaster = 96, kCells = 32, kMaxMasks = 1200;
// Masks smaller than this (the compiler drops leaves below 1e-5) are not compared: which side of the threshold a
// sliver falls on may differ by the tolerance.
constexpr double kMinimumArea = 3e-5;

struct FrameDigest {
    u32 document, frame, compiled, masks;
};
struct MaskDigest {
    u32 fill, segments;
    float bounds[4];
    float area, length;
    u8 cells[kCells * kCells];
};
static_assert(sizeof(MaskDigest) == 32 + kCells * kCells);

struct Corpus {
    Arena* arena;
    u32 count;
    // The k-th document's name and bytes; false when it cannot be read.
    bool (*get)(Corpus*, u32, const char**, ConstSlice<u8>*) noexcept;
    const char** paths;
};

bool from_embedded(Corpus*, u32 index, const char** name, ConstSlice<u8>* bytes) noexcept {
    *name = kDocuments[index].name;
    *bytes = {kDocuments[index].data, kDocuments[index].bytes};
    return true;
}

bool read_whole(Arena* arena, const char* path, ConstSlice<u8>* out) noexcept {
    wchar_t wide[520];
    u32 i = 0;
    for (; path[i] && i < 519; ++i)
        wide[i] = static_cast<wchar_t>(static_cast<u8>(path[i]));
    wide[i] = 0;
    const FileHandle file = file_open(wide, {FileAccess::read, FileCreate::existing, kFileShareRead, 0});
    if (file == INVALID_HANDLE_VALUE)
        return false;
    u64 size = 0;
    u8* data = nullptr;
    bool ok = file_size(file, &size) && size < (usize{1} << 31);
    if (ok)
        data = arena_allocate<u8>(arena, static_cast<u32>(size) + 1);
    u32 done = 0;
    while (ok && data && done < size) {
        u32 got = 0;
        ok = file_read(file, data + done, static_cast<u32>(size) - done, &got) && got;
        done += got;
    }
    file_close(file);
    if (!ok || !data)
        return false;
    *out = {data, static_cast<u32>(size)};
    return true;
}

bool from_list(Corpus* corpus, u32 index, const char** name, ConstSlice<u8>* bytes) noexcept {
    *name = corpus->paths[index];
    return read_whole(corpus->arena, corpus->paths[index], bytes);
}

// ---- digesting a compiled program -----------------------------------------------------------------------------

lottie_raster::Edge* g_edges;
u32 g_edge_capacity;

bool digest_mask(Arena* arena, const ColrPaintProgram& program, u32 job_at, MaskDigest* out) noexcept {
    const auto* job = reinterpret_cast<const ColrMask*>(program.storage.base + job_at);
    const auto* segments = reinterpret_cast<const float*>(program.storage.base + job->segments);
    if (job->segment_count > g_edge_capacity)
        return false;
    lottie_raster::Edges edges{g_edges, 0, g_edge_capacity, 0, 0, 0, 0};
    double length = 0;
    for (u32 i = 0; i < job->segment_count; ++i) {
        const float* s = segments + i * 4;
        edges.add(s[0], s[1], s[2], s[3]);
        const double dx = s[2] - s[0], dy = s[3] - s[1];
        length += __builtin_elementwise_sqrt(dx * dx + dy * dy);
    }
    static lottie_raster::Coverage<kRaster> coverage;
    if (!lottie_raster::rasterize<kRaster>(arena, edges, 1.f, job->fill == 0, &coverage))
        return false;
    out->fill = job->fill;
    out->segments = job->segment_count;
    for (u32 i = 0; i < 4; ++i)
        out->bounds[i] = job->bounds[i];
    out->area = coverage.total / (kRaster * kRaster);
    out->length = static_cast<float>(length);
    constexpr u32 factor = kRaster / kCells;
    for (u32 y = 0; y < kCells; ++y)
        for (u32 x = 0; x < kCells; ++x) {
            u32 sum = 0;
            for (u32 dy = 0; dy < factor; ++dy)
                for (u32 dx = 0; dx < factor; ++dx)
                    sum += coverage.cells[(y * factor + dy) * kRaster + x * factor + dx];
            out->cells[y * kCells + x] = static_cast<u8>((sum + factor * factor / 2) / (factor * factor));
        }
    return true;
}

// The masks of a program in paint order; false when one cannot be digested.
bool digest_program(Arena* arena, const ColrPaintProgram& program, MaskDigest* masks, u32* count) noexcept {
    *count = 0;
    for (u32 at = program.first; at != ~0u && at < program.storage.offset;) {
        const auto* op = reinterpret_cast<const ColrPaintOp*>(program.storage.base + at);
        u32 job = ~0u;
        if (op->kind == 4)
            job = op->data;
        else if (op->kind >= 8 && op->kind <= 11)
            job = op->reserved;
        if (job != ~0u) {
            if (*count == kMaxMasks || !digest_mask(arena, program, job, &masks[*count]))
                return false;
            ++*count;
        }
        at = op->next;
    }
    return true;
}

// ---- comparing digests --------------------------------------------------------------------------------------

struct Tally {
    u32 frames, failed_frames, masks;
    double worst_area, worst_area_ratio;
    u32 worst_level, worst_over;
};

bool g_report;

struct Buffer {
    u8* data;
    usize size, capacity;
};
bool append(Buffer* buffer, const void* bytes, usize count) noexcept {
    if (buffer->size + count > buffer->capacity)
        return false;
    copy_bytes(buffer->data + buffer->size, bytes, static_cast<u32>(count));
    buffer->size += count;
    return true;
}

void print_frame(const char* name, u32 frame, const char* what) noexcept {
    test_write_text("  ");
    test_write_text(name);
    test_write_text(" frame ");
    test_write_u64(frame);
    test_write_text(": ");
    test_write_line(what);
}

// `expected` is the reference's frame, `actual` this build's.
bool similar(const char* name, const FrameDigest& expected_frame, const MaskDigest* expected, const FrameDigest& actual_frame,
             const MaskDigest* actual, Tally* tally) noexcept {
    ++tally->frames;
    if (expected_frame.compiled != actual_frame.compiled) {
        print_frame(name, actual_frame.frame, expected_frame.compiled ? "compiles in the reference only" : "compiles here only");
        return false;
    }
    if (!actual_frame.compiled)
        return true;
    u32 expected_index[kMaxMasks], actual_index[kMaxMasks], expected_count = 0, actual_count = 0;
    for (u32 i = 0; i < expected_frame.masks; ++i)
        if (expected[i].area >= kMinimumArea)
            expected_index[expected_count++] = i;
    for (u32 i = 0; i < actual_frame.masks; ++i)
        if (actual[i].area >= kMinimumArea)
            actual_index[actual_count++] = i;
    // A shape a hair above the compiler's area threshold on one side may be a hair below it on the other: a shape that
    // is small and has no partner is skipped; any other without one is a difference.
    constexpr double kBorder = 4e-5;
    const auto close = [](const MaskDigest& a, const MaskDigest& b) noexcept {
        for (u32 c = 0; c < 4; ++c) {
            const float d = a.bounds[c] - b.bounds[c];
            if (d > .004f || d < -.004f)
                return false;
        }
        return true;
    };
    bool ok = true;
    u32 i = 0, j = 0;
    while (i < expected_count || j < actual_count) {
        const MaskDigest* first = i < expected_count ? &expected[expected_index[i]] : nullptr;
        const MaskDigest* second = j < actual_count ? &actual[actual_index[j]] : nullptr;
        if (!(first && second && close(*first, *second))) {
            if (first && first->area < kBorder) {
                ++i;
                continue;
            }
            if (second && second->area < kBorder) {
                ++j;
                continue;
            }
            print_frame(name, actual_frame.frame, "the painted shapes differ");
            test_write_metric("    reference shapes: ", expected_count);
            test_write_metric("    here:             ", actual_count);
            return false;
        }
        const MaskDigest& a = *first;
        const MaskDigest& b = *second;
        ++tally->masks;
        const double length = a.length > b.length ? a.length : b.length;
        // Direct2D flattens combinations to .1 of 512 units and widens strokes to the same; areas move by that along
        // the outline.
        const double slack = 4e-4 * length + 1.5e-5 + .003 * (a.area > b.area ? a.area : b.area);
        double difference = static_cast<double>(a.area) - b.area;
        difference = difference < 0 ? -difference : difference;
        u32 worst = 0, over = 0;
        for (u32 i = 0; i < kCells * kCells; ++i) {
            const i32 d = static_cast<i32>(a.cells[i]) - b.cells[i];
            const u32 m = static_cast<u32>(d < 0 ? -d : d);
            worst = m > worst ? m : worst;
            over += m > 12;
        }
        tally->worst_area = difference > tally->worst_area ? difference : tally->worst_area;
        const double ratio = difference / slack;
        tally->worst_area_ratio = ratio > tally->worst_area_ratio ? ratio : tally->worst_area_ratio;
        tally->worst_level = worst > tally->worst_level ? worst : tally->worst_level;
        tally->worst_over = over > tally->worst_over ? over : tally->worst_over;
        if (difference > slack || worst > 48 || over > 6) {
            print_frame(name, actual_frame.frame, "a shape differs");
            test_write_metric("    shape: ", i);
            test_write_metric("    area difference x1e9: ", static_cast<u64>(difference * 1e9));
            test_write_metric("    allowed x1e9:         ", static_cast<u64>(slack * 1e9));
            test_write_metric("    worst cell level: ", worst);
            test_write_metric("    cells over 12 levels: ", over);
            ok = false;
        }
        ++i;
        ++j;
    }
    return ok;
}

// ---- running ------------------------------------------------------------------------------------------------

ColrPaintProgram g_program;
MaskDigest g_masks[kMaxMasks];

bool compile(ConstSlice<u8> json, u32 frame, LottieTiming* timing) noexcept {
    return lottie_program_compile_frame(
        json, frame, 160, timing, +[](void* program) noexcept { return static_cast<ColrPaintProgram*>(program); },
        &g_program);
}

// One document at up to three frames: the first, the middle and the last.
template <typename Visit>
bool each_frame(Arena* arena, u32 document, ConstSlice<u8> json, Visit&& visit) noexcept {
    LottieTiming timing{};
    const bool opened = compile(json, 0, &timing);
    u32 frames[3]{0, timing.frames / 2, timing.frames ? timing.frames - 1 : 0}, count = 0;
    for (u32 i = 0; i < 3; ++i) {
        bool repeated = false;
        for (u32 k = 0; k < count; ++k)
            repeated |= frames[k] == frames[i];
        if (!repeated)
            frames[count++] = frames[i];
    }
    bool ok = true;
    for (u32 i = 0; i < count; ++i) {
        FrameDigest header{document, frames[i], 0, 0};
        if (opened && compile(json, frames[i], &timing)) {
            u32 masks = 0;
            if (digest_program(arena, g_program, g_masks, &masks)) {
                header.compiled = 1;
                header.masks = masks;
            } else {
                ok = false;
            }
        }
        arena_release(&g_program.storage);
        g_program = {};
        ok = visit(header) && ok;
    }
    return ok;
}

bool capture_corpus(Arena* arena, Corpus* corpus, Buffer* out) noexcept {
    bool ok = true;
    for (u32 i = 0; i < corpus->count; ++i) {
        const char* name;
        ConstSlice<u8> json{};
        if (!corpus->get(corpus, i, &name, &json))
            continue;
        ok = each_frame(arena, i, json, [&](const FrameDigest& header) noexcept {
                 return append(out, &header, sizeof(header)) &&
                        append(out, g_masks, static_cast<usize>(header.masks) * sizeof(MaskDigest));
             }) && ok;
    }
    return ok;
}

bool compare_corpus(Arena* arena, Corpus* corpus, ConstSlice<u8> reference, Tally* tally) noexcept {
    bool ok = true;
    usize at = 0;
    u32 failures = 0;
    for (u32 i = 0; i < corpus->count; ++i) {
        const char* name;
        ConstSlice<u8> json{};
        if (!corpus->get(corpus, i, &name, &json)) {
            test_write_text("cannot read ");
            test_write_line(name);
            ok = false;
            continue;
        }
        ok = each_frame(arena, i, json, [&](const FrameDigest& actual) noexcept {
                 if (at + sizeof(FrameDigest) > reference.count)
                     return false;
                 FrameDigest expected;
                 copy_bytes(&expected, reference.data + at, sizeof(expected));
                 const usize bytes = static_cast<usize>(expected.masks) * sizeof(MaskDigest);
                 if (expected.document != actual.document || expected.frame != actual.frame ||
                     at + sizeof(FrameDigest) + bytes > reference.count || expected.masks > kMaxMasks) {
                     print_frame(name, actual.frame, "the reference does not hold this frame");
                     return false;
                 }
                 static MaskDigest reference_masks[kMaxMasks];
                 copy_bytes(reference_masks, reference.data + at + sizeof(FrameDigest), static_cast<u32>(bytes));
                 at += sizeof(FrameDigest) + bytes;
                 const bool good = similar(name, expected, reference_masks, actual, g_masks, tally);
                 failures += !good;
                 return good;
             }) && ok;
    }
    tally->failed_frames = failures;
    return ok && at == reference.count;
}

bool environment_flag(const char* name) noexcept {
    char value[8];
    return capture_io::environment(name, value, sizeof(value));
}

void print_tally(const Tally& t) noexcept {
    test_write_metric("frames compared: ", t.frames);
    test_write_metric("shapes compared: ", t.masks);
    test_write_metric("frames that differ: ", t.failed_frames);
    test_write_metric("worst area difference x1e9: ", static_cast<u64>(t.worst_area * 1e9));
    test_write_metric("worst area difference / allowed x1000: ", static_cast<u64>(t.worst_area_ratio * 1000));
    test_write_metric("worst cell level: ", t.worst_level);
    test_write_metric("most cells over 12 levels: ", t.worst_over);
}

// The paths of a list file, one per line.
bool read_list(Arena* arena, const char* path, Corpus* corpus) noexcept {
    ConstSlice<u8> text{};
    if (!read_whole(arena, path, &text))
        return false;
    auto* bytes = const_cast<u8*>(text.data);
    const char** paths = arena_allocate<const char*>(arena, 65536);
    if (!paths)
        return false;
    u32 count = 0;
    for (u32 i = 0; i < text.count && count < 65536;) {
        while (i < text.count && (bytes[i] == '\n' || bytes[i] == '\r'))
            ++i;
        if (i >= text.count)
            break;
        paths[count++] = reinterpret_cast<const char*>(bytes + i);
        while (i < text.count && bytes[i] != '\n' && bytes[i] != '\r')
            ++i;
        if (i < text.count)
            bytes[i++] = 0;
    }
    bytes[text.count] = 0;
    corpus->paths = paths;
    corpus->count = count;
    return true;
}

bool run() noexcept {
    Arena arena{};
    if (!arena_initialize(&arena, usize{3} << 30, 0, usize{1} << 20))
        return false;
    g_report = environment_flag("MAICORD_LOTTIE_REPORT");
    g_edge_capacity = 1 << 20;
    g_edges = arena_allocate<lottie_raster::Edge>(&arena, g_edge_capacity);
    if (!g_edges)
        return false;
    u32 failures = 0;
    char path[520], compare_path[520];
    Corpus embedded{&arena, kDocumentCount, from_embedded, nullptr};
    Buffer out{};
    Tally tally{};
    if (capture_io::environment("MAICORD_LOTTIE_PROGRAM_CAPTURE", path, sizeof(path))) {
        out = {static_cast<u8*>(arena_allocate(&arena, usize{64} << 20, 16)), 0, usize{64} << 20};
        const bool ok = out.data && capture_corpus(&arena, &embedded, &out) &&
                        capture_io::write_file(path, out.data, static_cast<u32>(out.size));
        test_write_line(ok ? "PASS captured the compiled corpus" : "FAIL the capture failed");
        return ok;
    }
    char list[520];
    if (capture_io::environment("MAICORD_LOTTIE_CORPUS_LIST", list, sizeof(list))) {
        Corpus local{&arena, 0, from_list, nullptr};
        if (!read_list(&arena, list, &local))
            return false;
        if (capture_io::environment("MAICORD_LOTTIE_CORPUS_CAPTURE", path, sizeof(path))) {
            out = {static_cast<u8*>(arena_allocate(&arena, usize{1} << 30, 16)), 0, usize{1} << 30};
            const bool ok = out.data && capture_corpus(&arena, &local, &out) &&
                            capture_io::write_file(path, out.data, static_cast<u32>(out.size));
            test_write_metric("documents digested: ", local.count);
            test_write_line(ok ? "PASS captured the corpus" : "FAIL the capture failed");
            return ok;
        }
        ConstSlice<u8> reference{};
        if (!capture_io::environment("MAICORD_LOTTIE_CORPUS_COMPARE", compare_path, sizeof(compare_path)) ||
            !read_whole(&arena, compare_path, &reference))
            return false;
        test_check(&failures, compare_corpus(&arena, &local, reference, &tally), "the corpus matches its reference");
        print_tally(tally);
        return failures == 0;
    }
    const ConstSlice<u8> fixture{kFixture, sizeof(kFixture)};
    test_check(&failures, sizeof(kFixture) > 1, "the fixture is present");
    if (sizeof(kFixture) > 1)
        test_check(&failures, compare_corpus(&arena, &embedded, fixture, &tally), "the corpus matches Direct2D's");
    if (g_report || failures)
        print_tally(tally);
    return failures == 0;
}
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_lottie_program_entry) {
    maicord::test_exit(maicord::run() ? 0 : 1, "lottie program");
}
