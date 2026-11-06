#include "maicord/render/effects/vector_geometry.hpp"
#include "../../gpu/capture_io.hpp"
#include "lottie_geometry_raster.hpp"

// The vector geometry engine of the Lottie compiler.
//
// A fixed list of cases (primitives, paths that cross themselves, the four boolean combinations of shapes that touch,
// coincide and are tangent, strokes of every cap, join and width) is run on the engine and compared with what Direct2D
// answered for them when the compiler ran on it (tests/render/effects/golden/lottie_geometry.bin): the area of each
// result and its coverage on a grid, within the flattening tolerance. Coordinates are in 512 units, the flattening
// tolerances are the ones the Lottie compiler passes at that size (0.1 for combines and strokes, 0.05 for areas).
// Then cases with exact answers: areas of combinations of rectangles that share, overlap along or touch at edges, far
// from the origin and across a huge range; self-crossing, tangent and zero-area outlines; the identities between the
// four combinations of curved shapes; and chains of combinations, where each takes the last result as an operand.
namespace maicord {
namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
alignas(16) constexpr u8 kFixture[] = {
#embed "golden/lottie_geometry.bin" if_empty(0)
};
#pragma clang diagnostic pop

constexpr u32 kGrid = 64;
constexpr float kExtent = 512;
constexpr float kCombineTolerance = .1f, kAreaTolerance = .05f;

// ---- the cases -------------------------------------------------------------------------------------------------

struct Spec {
    struct Figure {
        float sx, sy;
        u32 count;
        bool closed;
    };
    Figure figures[4];
    u32 figure_count;
    float points[4][48][6];
    Spec& begin(float x, float y) noexcept {
        figures[figure_count++] = {x, y, 0, false};
        cx = x;
        cy = y;
        return *this;
    }
    Spec& curve(float x1, float y1, float x2, float y2, float x, float y) noexcept {
        Figure& f = figures[figure_count - 1];
        const float p[6]{x1, y1, x2, y2, x, y};
        for (u32 i = 0; i < 6; ++i)
            points[figure_count - 1][f.count][i] = p[i];
        ++f.count;
        cx = x;
        cy = y;
        return *this;
    }
    // A straight segment the way the Lottie compiler writes one: a cubic whose handles are its ends.
    Spec& line(float x, float y) noexcept { return curve(cx, cy, x, y, x, y); }
    Spec& end(bool closed) noexcept {
        figures[figure_count - 1].closed = closed;
        return *this;
    }
    float cx, cy;
};

struct Rng {
    u32 state;
    float next(float low, float high) noexcept {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return low + (high - low) * static_cast<float>(state & 0xffffff) / 16777216.f;
    }
};

// Sine and cosine of an angle in [-pi, pi] by their series: the tests have no math library.
void sine_cosine(float a, float* sine, float* cosine) noexcept {
    const float z = a * a;
    *sine = a * (1 + z * (-1.f / 6 + z * (1.f / 120 + z * (-1.f / 5040 + z * (1.f / 362880 + z * (-1.f / 39916800))))));
    *cosine = 1 + z * (-.5f + z * (1.f / 24 + z * (-1.f / 720 + z * (1.f / 40320 + z * (-1.f / 3628800 + z / 479001600)))));
}

// A smooth closed curve through `count` points around (cx, cy) at radii between low and high.
void blob(Spec& s, float cx, float cy, float low, float high, u32 count, u32 seed) noexcept {
    Rng rng{seed};
    float px[16], py[16];
    for (u32 i = 0; i < count; ++i) {
        const float angle = 6.2831853f * static_cast<float>(i) / count, r = rng.next(low, high);
        float sine, cosine;
        sine_cosine(angle > 3.1415927f ? angle - 6.2831853f : angle, &sine, &cosine);
        px[i] = cx + r * cosine;
        py[i] = cy + r * sine;
    }
    s.begin(px[0], py[0]);
    for (u32 i = 0; i < count; ++i) {
        const u32 p = (i + count - 1) % count, n = (i + 1) % count, m = (i + 2) % count;
        s.curve(px[i] + (px[n] - px[p]) / 6, py[i] + (py[n] - py[p]) / 6, px[n] - (px[m] - px[i]) / 6,
                py[n] - (py[m] - py[i]) / 6, px[n], py[n]);
    }
    s.end(true);
}

void circle(Spec& s, float cx, float cy, float r, bool reversed) noexcept {
    const float k = r * .5522847f;
    if (!reversed) {
        s.begin(cx + r, cy).curve(cx + r, cy + k, cx + k, cy + r, cx, cy + r).curve(cx - k, cy + r, cx - r, cy + k, cx - r, cy)
            .curve(cx - r, cy - k, cx - k, cy - r, cx, cy - r).curve(cx + k, cy - r, cx + r, cy - k, cx + r, cy).end(true);
    } else {
        s.begin(cx + r, cy).curve(cx + r, cy - k, cx + k, cy - r, cx, cy - r).curve(cx - k, cy - r, cx - r, cy - k, cx - r, cy)
            .curve(cx - r, cy + k, cx - k, cy + r, cx, cy + r).curve(cx + k, cy + r, cx + r, cy + k, cx + r, cy).end(true);
    }
}

void polygon(Spec& s, const float* xy, u32 count, bool closed) noexcept {
    s.begin(xy[0], xy[1]);
    for (u32 i = 1; i < count; ++i)
        s.line(xy[i * 2], xy[i * 2 + 1]);
    if (closed)
        s.line(xy[0], xy[1]);
    s.end(closed);
}

enum Shape : u32 {
    shape_circle, shape_ellipse, shape_rounded, shape_rect, shape_blob_a, shape_blob_b, shape_star, shape_eight,
    shape_c, shape_ring, shape_wave, shape_loop, shape_square, shape_sharp, shape_reversal, shape_small_circle,
    shape_hook, shape_dup, shape_count
};

// What the generic code needs of a primitive: either one of the factory's own or a path.
struct Primitive {
    enum Kind : u8 { path, ellipse, rounded, rectangle } kind;
    float a, b, c, d, e, f;
    Spec spec;
};
void shape(u32 id, Primitive* out) noexcept {
    *out = {};
    out->kind = Primitive::path;
    Spec& s = out->spec;
    switch (id) {
    case shape_circle:
        out->kind = Primitive::ellipse;
        out->a = 256, out->b = 256, out->c = 150, out->d = 150;
        break;
    case shape_ellipse:
        out->kind = Primitive::ellipse;
        out->a = 256, out->b = 256, out->c = 200, out->d = 90;
        break;
    case shape_rounded:
        out->kind = Primitive::rounded;
        out->a = 60, out->b = 80, out->c = 450, out->d = 400, out->e = 60, out->f = 60;
        break;
    case shape_rect:
        out->kind = Primitive::rectangle;
        out->a = 100, out->b = 100, out->c = 400, out->d = 400;
        break;
    case shape_blob_a:
        blob(s, 256, 256, 70, 170, 7, 11);
        break;
    case shape_blob_b:
        blob(s, 300, 230, 60, 150, 9, 23);
        break;
    case shape_star: {
        float xy[20];
        for (u32 i = 0; i < 10; ++i) {
            const float r = i & 1 ? 70.f : 190.f;
            // Every 36 degrees: sines and cosines of 0, 36, ... 324 degrees, tabulated.
            static constexpr float sine[]{0.f, .5877853f, .9510565f, .9510565f, .5877853f, 0.f, -.5877853f,
                                          -.9510565f, -.9510565f, -.5877853f};
            static constexpr float cosine[]{1.f, .8090170f, .3090170f, -.3090170f, -.8090170f, -1.f, -.8090170f,
                                            -.3090170f, .3090170f, .8090170f};
            xy[i * 2] = 256 + r * sine[i];
            xy[i * 2 + 1] = 256 - r * cosine[i];
        }
        polygon(s, xy, 10, true);
        break;
    }
    case shape_eight:
        s.begin(256, 256).curve(340, 120, 460, 150, 460, 256).curve(460, 362, 340, 392, 256, 256)
            .curve(172, 120, 52, 150, 52, 256).curve(52, 362, 172, 392, 256, 256).end(true);
        break;
    case shape_c:
        s.begin(400, 120).curve(300, 40, 120, 80, 100, 256).curve(80, 430, 300, 470, 410, 390).line(360, 330)
            .curve(290, 380, 190, 370, 180, 256).curve(170, 150, 300, 130, 350, 180).end(true);
        break;
    case shape_ring:
        circle(s, 256, 256, 160, false);
        circle(s, 256, 256, 80, true);
        break;
    case shape_wave:
        s.begin(40, 400).curve(140, 100, 200, 500, 300, 300).curve(400, 100, 420, 100, 480, 260).end(false);
        break;
    case shape_loop:
        circle(s, 256, 256, 6, false);
        break;
    case shape_square: {
        const float xy[]{30, 30, 150, 30, 150, 150, 30, 150};
        polygon(s, xy, 4, true);
        break;
    }
    case shape_sharp: {
        const float xy[]{60, 60, 450, 100, 80, 120};
        polygon(s, xy, 3, false);
        break;
    }
    case shape_hook: {
        // Four smooth vertices of a lens whose ends are hairpins: the handles at the tips are tiny beside the ones
        // that sweep the lens, so the curve turns about a hundred and fifty degrees within a pixel of a vertex.
        const float v[4][2]{{380, 280}, {256, 232}, {132, 280}, {256, 233}};
        const float in[4][2]{{-20, -30}, {60, .6f}, {-2, 3.5f}, {-60, -.6f}};
        const float out[4][2]{{2, 3}, {-60, -.6f}, {20, -35}, {60, .6f}};
        s.begin(v[0][0], v[0][1]);
        for (u32 i = 0; i < 4; ++i) {
            const u32 n = (i + 1) % 4;
            s.curve(v[i][0] + out[i][0], v[i][1] + out[i][1], v[n][0] + in[n][0], v[n][1] + in[n][1], v[n][0], v[n][1]);
        }
        s.end(true);
        break;
    }
    case shape_dup: {
        // An open path out and back whose turn is a zero-length segment: its two vertices coincide.
        s.begin(100, 100).curve(100, 100, 300, 100, 300, 100).curve(300, 100, 300, 100, 300, 100)
            .curve(300, 100, 100, 100, 100, 100).end(false);
        break;
    }
    case shape_reversal: {
        const float xy[]{100, 100, 400, 100, 100, 100};
        polygon(s, xy, 3, false);
        break;
    }
    default:
        out->kind = Primitive::ellipse;
        out->a = 256, out->b = 256, out->c = 20, out->d = 20;
        break;
    }
}

enum class Op : u8 { shape, combine, widen };
struct Case {
    Op op;
    u32 first, second;
    u32 mode;            // combine mode, or the cap * 3 + join of a stroke
    float width, miter;  // a stroke
    // The second operand moved: translation and a scale and rotation in 1/1000, as a matrix.
    float matrix[6];
    const char* name;
};

struct Combo {
    u32 first, second, move;
};
// The second operand of a combination is moved by one of these: nothing, a shift that leaves the rectangles sharing an
// edge and the circles tangent, one that overlaps them, a rotation, a near coincidence and a nearer one.
constexpr float kMoves[][6] = {{1, 0, 0, 1, 0, 0},   {1, 0, 0, 1, 300, 0}, {1, 0, 0, 1, 150, 100},
                               {.8660254f, .5f, -.5f, .8660254f, 0, 0}, {1, 0, 0, 1, 1, 0}, {1, 0, 0, 1, .01f, 0}};
constexpr Combo kCombos[] = {
    {shape_circle, shape_ellipse, 0},  {shape_circle, shape_rect, 0},     {shape_blob_a, shape_blob_b, 0},
    {shape_blob_b, shape_blob_a, 0},   {shape_star, shape_circle, 0},     {shape_eight, shape_circle, 0},
    {shape_c, shape_blob_a, 0},        {shape_ring, shape_circle, 0},     {shape_ring, shape_blob_a, 0},
    {shape_rounded, shape_star, 0},    {shape_rect, shape_rect, 0},       {shape_rect, shape_rect, 1},
    {shape_rect, shape_rect, 2},       {shape_rect, shape_ellipse, 3},    {shape_circle, shape_circle, 1},
    {shape_circle, shape_circle, 0},   {shape_blob_a, shape_blob_a, 4},   {shape_star, shape_star, 5}};
constexpr u32 kComboCount = sizeof(kCombos) / sizeof(kCombos[0]);

struct Stroke {
    u32 shape, mode;
    float width, miter;
};
// mode is cap * 3 + join, in Direct2D's numbering: caps flat, square, round; joins miter, bevel, round.
constexpr Stroke kStrokes[] = {
    {shape_sharp, 0, 16, 1.5f},    {shape_sharp, 0, 16, 4},      {shape_sharp, 0, 16, 20},    {shape_wave, 0, 24, 4},
    {shape_wave, 1, 24, 4},        {shape_wave, 2, 24, 4},       {shape_wave, 3, 24, 4},      {shape_wave, 4, 24, 4},
    {shape_wave, 5, 24, 4},        {shape_wave, 6, 24, 4},       {shape_wave, 7, 24, 4},      {shape_wave, 8, 24, 4},
    {shape_wave, 8, 2, 4},         {shape_wave, 8, 80, 4},       {shape_star, 0, 20, 4},      {shape_star, 8, 20, 4},
    {shape_star, 4, 60, 4},        {shape_blob_a, 8, 6, 4},      {shape_blob_a, 8, 40, 4},    {shape_blob_a, 8, 160, 4},
    {shape_eight, 8, 30, 4},       {shape_loop, 8, 20, 4},       {shape_square, 7, 10, 4},    {shape_reversal, 8, 30, 4},
    {shape_reversal, 4, 30, 4},    {shape_reversal, 3, 30, 4},   {shape_ring, 8, 14, 4},      {shape_small_circle, 8, 10, 4},
    {shape_hook, 0, 20, 4},        {shape_hook, 0, 20, 10},      {shape_hook, 1, 20, 4},      {shape_dup, 0, 24, 4},
    {shape_dup, 0, 24, 10},        {shape_dup, 8, 24, 4}};
constexpr u32 kStrokeCount = sizeof(kStrokes) / sizeof(kStrokes[0]);

constexpr u32 kCaseCount = shape_count + kComboCount * 4 + kStrokeCount;

void describe(u32 index, Case* c) noexcept {
    *c = {};
    if (index < shape_count) {
        c->op = Op::shape;
        c->first = index;
        c->name = "primitive";
    } else if (index < shape_count + kComboCount * 4) {
        const u32 at = index - shape_count;
        c->op = Op::combine;
        c->first = kCombos[at / 4].first;
        c->second = kCombos[at / 4].second;
        c->mode = at % 4;
        for (u32 i = 0; i < 6; ++i)
            c->matrix[i] = kMoves[kCombos[at / 4].move][i];
        c->name = "combination";
    } else {
        const Stroke& s = kStrokes[index - shape_count - kComboCount * 4];
        c->op = Op::widen;
        c->first = s.shape;
        c->mode = s.mode;
        c->width = s.width;
        c->miter = s.miter;
        c->name = "stroke";
    }
}

// ---- running the cases on the engine ------------------------------------------------------------------------------

using geometry::Geometry;
using geometry::Point;

// What a case is compared by. The fixture holds, per case after its count, area, length, figure and command counts
// and bounds (4 bytes each, 4 for the bounds) and the cells.
struct Record {
    float area, length;
    u8 cells[kGrid * kGrid];
};
constexpr u32 kRecordBytes = 4 + 4 + 4 + 4 + 16 + kGrid * kGrid;

// A path geometry of the figures of a spec.
Geometry* path_of(geometry::Factory* factory, const Spec& spec) noexcept {
    Geometry* g = geometry::geometry_create(factory);
    for (u32 f = 0; g && f < spec.figure_count; ++f) {
        const Spec::Figure& figure = spec.figures[f];
        if (!geometry::geometry_begin(g, {figure.sx, figure.sy}, false) ||
            !geometry::geometry_append(g, reinterpret_cast<const Point*>(spec.points[f]), figure.count * 3))
            return nullptr;
        geometry::geometry_end(g, figure.closed);
    }
    return g;
}

Geometry* make(geometry::Factory* factory, const Primitive& p) noexcept {
    switch (p.kind) {
    case Primitive::ellipse:
        return geometry::geometry_rounded_rectangle(factory, p.a - p.c, p.b - p.d, p.a + p.c, p.b + p.d, p.c, p.d);
    case Primitive::rounded:
        return geometry::geometry_rounded_rectangle(factory, p.a, p.b, p.c, p.d, p.e, p.f);
    case Primitive::rectangle:
        return geometry::geometry_rounded_rectangle(factory, p.a, p.b, p.c, p.d, 0, 0);
    default:
        return path_of(factory, p.spec);
    }
}

// Measures `geometry` into a record: its area, outline length, size and coverage.
bool measure(Arena* arena, lottie_raster::Edge* storage, const Geometry* geometry, Record* r) noexcept {
    double area = 0;
    if (!geometry::geometry_area(geometry, nullptr, kAreaTolerance, &area))
        return false;
    lottie_raster::Edges edges{storage, 0, 1 << 20, 0, 0, 0, 0};
    double length = 0;
    const Point* p = geometry->points;
    for (u32 f = 0; f < geometry->figure_count; ++f) {
        const geometry::Figure& figure = geometry->figures[f];
        edges.begin(p[figure.first].x, p[figure.first].y);
        const u32 step = figure.polyline ? 1 : 3;
        for (u32 at = figure.first + step; at < figure.end; at += step) {
            const double dx = p[at].x - edges.cx, dy = p[at].y - edges.cy;
            length += __builtin_elementwise_sqrt(dx * dx + dy * dy);
            if (step == 1)
                edges.line(p[at].x, p[at].y);
            else
                edges.cubic(p[at - 2].x, p[at - 2].y, p[at - 1].x, p[at - 1].y, p[at].x, p[at].y, .02f);
        }
        edges.close();
    }
    lottie_raster::Coverage<kGrid> coverage;
    if (!lottie_raster::rasterize<kGrid>(arena, edges, kExtent, false, &coverage))
        return false;
    r->area = static_cast<float>(area);
    r->length = static_cast<float>(length);
    copy_bytes(r->cells, coverage.cells, sizeof(coverage.cells));
    return true;
}

bool run_case(geometry::Factory* factory, Arena* arena, lottie_raster::Edge* storage, const Case& c,
              Record* record) noexcept {
    Primitive first, second;
    shape(c.first, &first);
    Geometry* a = make(factory, first);
    if (!a)
        return false;
    if (c.op == Op::shape)
        return measure(arena, storage, a, record);
    if (c.op == Op::widen) {
        const geometry::StrokeStyle style{c.width, c.miter, static_cast<geometry::CapStyle>(c.mode / 3),
                                          static_cast<geometry::LineJoin>(c.mode % 3)};
        Geometry* widened = geometry::geometry_stroke(a, style, kCombineTolerance);
        return widened && measure(arena, storage, widened, record);
    }
    shape(c.second, &second);
    Geometry* b = make(factory, second);
    const geometry::Matrix m{c.matrix[0], c.matrix[1], c.matrix[2], c.matrix[3], c.matrix[4], c.matrix[5]};
    Geometry* placed = b ? geometry::geometry_transform(b, m) : nullptr;
    Geometry* combined =
        placed ? geometry::geometry_combine(a, placed, static_cast<geometry::CombineMode>(c.mode), kCombineTolerance)
               : nullptr;
    return combined && measure(arena, storage, combined, record);
}

// ---- comparison ------------------------------------------------------------------------------------------------

struct Totals {
    u32 failures, compared;
};

u32 g_report;

void print_case(u32 index, const Case& c, const char* what) noexcept {
    test_write_text("  case ");
    test_write_u64(index);
    test_write_text(" (");
    test_write_text(c.name);
    test_write_text(", op ");
    test_write_u64(static_cast<u64>(c.op));
    test_write_text(", shapes ");
    test_write_u64(c.first);
    test_write_text("/");
    test_write_u64(c.second);
    test_write_text(", mode ");
    test_write_u64(c.mode);
    test_write_text("): ");
    test_write_line(what);
}

// `expected` is Direct2D's record for the case, `actual` the engine's. Areas may differ by what the flattening
// tolerance moves along the outline; coverage by a few levels where an outline moves by it.
bool similar(u32 index, const Case& c, const Record& expected, const Record& actual, u32 allowed_levels) noexcept {
    bool ok = true;
    const double length = expected.length > actual.length ? expected.length : actual.length;
    const double area_slack = kCombineTolerance * length + .002 * (expected.area > 1 ? expected.area : 1) + 1;
    double difference = static_cast<double>(expected.area) - actual.area;
    difference = difference < 0 ? -difference : difference;
    if (difference > area_slack) {
        print_case(index, c, "area differs");
        test_write_metric("    expected area x1000: ", static_cast<u64>(expected.area * 1000));
        test_write_metric("    actual area x1000:   ", static_cast<u64>(actual.area * 1000));
        test_write_metric("    allowed x1000:       ", static_cast<u64>(area_slack * 1000));
        ok = false;
    }
    u32 worst = 0, over = 0;
    for (u32 i = 0; i < kGrid * kGrid; ++i) {
        const i32 d = static_cast<i32>(expected.cells[i]) - actual.cells[i];
        const u32 m = static_cast<u32>(d < 0 ? -d : d);
        worst = m > worst ? m : worst;
        over += m > allowed_levels;
    }
    if (worst > allowed_levels * 3 || over > 8) {
        print_case(index, c, "coverage differs");
        test_write_metric("    worst level: ", worst);
        test_write_metric("    cells over the allowance: ", over);
        if (g_report) {
            const Record* records[]{&expected, &actual};
            for (u32 k = 0; k < 2; ++k) {
                test_write_line(k ? "    engine:" : "    reference:");
                for (u32 y = 0; y < kGrid; ++y) {
                    char line[kGrid + 1];
                    for (u32 x = 0; x < kGrid; ++x)
                        line[x] = " .:-=+*#%@"[records[k]->cells[y * kGrid + x] * 9 / 255];
                    line[kGrid] = 0;
                    test_write_line(line);
                }
            }
        }
        ok = false;
    } else if (g_report) {
        print_case(index, c, "ok");
        test_write_metric("    worst level: ", worst);
    }
    return ok;
}

bool environment_flag(const char* name) noexcept {
    char value[8];
    return capture_io::environment(name, value, sizeof(value));
}

bool run_all(Arena* arena, lottie_raster::Edge* storage, Record* out) noexcept {
    geometry::Factory* factory = nullptr;
    if (!geometry::factory_create(&factory))
        return false;
    bool ok = true;
    for (u32 i = 0; i < kCaseCount; ++i) {
        Case c;
        describe(i, &c);
        if (!run_case(factory, arena, storage, c, &out[i])) {
            print_case(i, c, "the engine failed");
            ok = false;
        }
    }
    geometry::factory_release(factory);
    return ok;
}

Record g_engine[kCaseCount];
lottie_raster::Edge g_edges[1 << 20];

void unpack(u32 index, Record* r) noexcept {
    const u8* at = kFixture + 4 + static_cast<usize>(index) * kRecordBytes;
    copy_bytes(&r->area, at, 4);
    copy_bytes(&r->length, at + 4, 4);
    copy_bytes(r->cells, at + 32, kGrid * kGrid);
}

// ---- cases with exact answers ----------------------------------------------------------------------------------

struct Exact {
    geometry::Factory* factory;
    u32 failures;

    void expect(const char* what, double value, double expected, double slack) noexcept {
        if (value >= expected - slack && value <= expected + slack)
            return;
        ++failures;
        test_write_text("exact case failed: ");
        test_write_line(what);
        test_write_metric("    value x1000:    ", static_cast<u64>((value < 0 ? -value : value) * 1000));
        test_write_metric("    expected x1000: ", static_cast<u64>((expected < 0 ? -expected : expected) * 1000));
    }
    Geometry* rect(float left, float top, float right, float bottom) noexcept {
        return geometry::geometry_rounded_rectangle(factory, left, top, right, bottom, 0, 0);
    }
    Geometry* circle(float cx, float cy, float r) noexcept {
        return geometry::geometry_rounded_rectangle(factory, cx - r, cy - r, cx + r, cy + r, r, r);
    }
    // One figure through the points, each segment a cubic with its handles on its ends.
    Geometry* polygon(const Point* points, u32 count, bool closed) noexcept {
        Geometry* g = geometry::geometry_create(factory);
        if (!g || !geometry::geometry_begin(g, points[0], false))
            return nullptr;
        for (u32 i = 1; i < count; ++i) {
            const Point segment[]{points[i - 1], points[i], points[i]};
            if (!geometry::geometry_append(g, segment, 3))
                return nullptr;
        }
        geometry::geometry_end(g, closed);
        return g;
    }
    // NaN when there is no geometry or the engine fails, so that no expectation holds.
    static double area(const Geometry* g, float tolerance = kAreaTolerance) noexcept {
        double value = 0;
        return g && geometry::geometry_area(g, nullptr, tolerance, &value) ? value : __builtin_nan("");
    }
    // An upper bound of the outline's length: the length of its control polygon.
    static double outline(const Geometry* g) noexcept {
        double length = 0;
        for (u32 i = 1; g && i < g->point_count; ++i) {
            const double dx = g->points[i].x - g->points[i - 1].x, dy = g->points[i].y - g->points[i - 1].y;
            length += __builtin_elementwise_sqrt(dx * dx + dy * dy);
        }
        return length;
    }
    static Geometry* combine(const Geometry* a, const Geometry* b, u32 mode,
                             float tolerance = kCombineTolerance) noexcept {
        return a && b ? geometry::geometry_combine(a, b, static_cast<geometry::CombineMode>(mode), tolerance) : nullptr;
    }
    static Geometry* stroke(const Geometry* g, const geometry::StrokeStyle& style) noexcept {
        return g ? geometry::geometry_stroke(g, style, kCombineTolerance) : nullptr;
    }
    // The areas of the union, intersection, difference either way and difference of `a` and `b`.
    void combinations(const char* what, const Geometry* a, const Geometry* b, const double (&expected)[4], double slack,
                      float tolerance = kCombineTolerance) noexcept {
        for (u32 mode = 0; mode < 4; ++mode)
            expect(what, area(combine(a, b, mode, tolerance), tolerance * .5f), expected[mode], slack);
    }
    // Whether `g` exists and has no figure.
    void empty(const char* what, const Geometry* g) noexcept { expect(what, g ? g->figure_count : -1.0, 0, 0); }
};

bool exact_cases() noexcept {
    Exact e{};
    if (!geometry::factory_create(&e.factory))
        return false;
    constexpr double pi = 3.14159265358979;
    e.expect("a circle", Exact::area(e.circle(256, 256, 100)), pi * 10000, 2000 * .05 * pi + 1);

    // Rectangles: overlapping, sharing an edge, overlapping along one, equal, inside on two edges, touching at a
    // corner, apart; far from the origin, a million units wide and a tenth of a unit wide.
    const Geometry* square = e.rect(0, 0, 100, 100);
    e.combinations("rectangles overlapping", square, e.rect(50, 50, 150, 150), {17500, 2500, 15000, 7500}, .5);
    e.combinations("rectangles sharing an edge", square, e.rect(100, 0, 200, 100), {20000, 0, 20000, 10000}, .5);
    e.combinations("rectangles overlapping along their edges", square, e.rect(50, 0, 150, 100),
                   {15000, 5000, 10000, 5000}, .5);
    e.combinations("equal rectangles", square, e.rect(0, 0, 100, 100), {10000, 10000, 0, 0}, .5);
    e.combinations("a rectangle inside on two edges", square, e.rect(0, 0, 50, 50), {10000, 2500, 7500, 7500}, .5);
    e.combinations("rectangles touching at a corner", square, e.rect(100, 100, 200, 200), {20000, 0, 20000, 10000}, .5);
    e.combinations("rectangles apart", square, e.rect(200, 0, 300, 100), {20000, 0, 20000, 10000}, .5);
    e.empty("the intersection of rectangles sharing an edge",
            Exact::combine(square, e.rect(100, 0, 200, 100), geometry::combine_intersect));
    e.empty("the intersection of rectangles apart",
            Exact::combine(square, e.rect(200, 0, 300, 100), geometry::combine_intersect));
    e.empty("the difference of equal rectangles",
            Exact::combine(square, e.rect(0, 0, 100, 100), geometry::combine_xor));
    e.combinations("rectangles far from the origin", e.rect(1e6f, 1e6f, 1e6f + 100, 1e6f + 100),
                   e.rect(1e6f + 50, 1e6f + 50, 1e6f + 150, 1e6f + 150), {17500, 2500, 15000, 7500}, .5);
    e.combinations("huge rectangles", e.rect(0, 0, 1e6f, 1e6f), e.rect(5e5f, 5e5f, 1.5e6f, 1.5e6f),
                   {1.75e12, .25e12, 1.5e12, .75e12}, 1e7, 1000);
    e.combinations("tiny rectangles", e.rect(0, 0, .1f, .1f), e.rect(.05f, .05f, .15f, .15f),
                   {.0175, .0025, .015, .0075}, 1e-6, 1e-4f);

    // Outlines that cross or run over themselves fill each point once; spikes and flat figures add nothing.
    const Point bowtie[]{{0, 0}, {100, 100}, {100, 0}, {0, 100}};
    e.expect("a bowtie", Exact::area(e.polygon(bowtie, 4, true)), 5000, .5);
    const Point twice[]{{0, 0}, {100, 0}, {100, 100}, {0, 100}, {0, 0}, {100, 0}, {100, 100}, {0, 100}};
    e.expect("a square drawn twice", Exact::area(e.polygon(twice, 8, true)), 10000, .5);
    const Point spike[]{{0, 0}, {100, 0}, {100, 100}, {150, 100}, {100, 100}, {0, 100}};
    const Geometry* spiked = e.polygon(spike, 6, true);
    e.expect("a square with a spike", Exact::area(spiked), 10000, .5);
    e.combinations("a square with a spike and the square", spiked, square, {10000, 10000, 0, 0}, .5);
    e.empty("the difference of a square with a spike and the square",
            Exact::combine(spiked, square, geometry::combine_xor));
    const Point flat[]{{-50, 50}, {200, 50}, {100, 50}};
    e.combinations("a flat figure across a square", square, e.polygon(flat, 3, true), {10000, 0, 10000, 10000}, .5);

    // Tangent circles, and a circle touching a square's four sides from inside.
    const Geometry *left = e.circle(156, 256, 100), *right = e.circle(356, 256, 100);
    const double disc = Exact::area(left);
    e.combinations("tangent circles", left, right, {2 * disc, 0, 2 * disc, disc}, 4 * pi * 100 * kCombineTolerance + 1);
    const Geometry* inscribed = e.circle(50, 50, 50);
    const double inner = Exact::area(inscribed);
    e.combinations("a circle inscribed in a square", square, inscribed, {10000, inner, 10000 - inner, 10000 - inner},
                   2 * pi * 50 * kCombineTolerance + 1);

    // A comb of 200 teeth across a band: 400 crossings.
    Point comb[3 + 200 * 4];
    u32 count = 0;
    comb[count++] = {0, 0};
    comb[count++] = {400, 0};
    comb[count++] = {400, 10};
    for (u32 i = 200; i-- > 0;) {
        const float x = 2.f * static_cast<float>(i);
        comb[count++] = {x + 1, 10};
        comb[count++] = {x + 1, 100};
        comb[count++] = {x, 100};
        comb[count++] = {x, 10};
    }
    e.combinations("a comb across a band", e.polygon(comb, count, true), e.rect(0, 50, 400, 60),
                   {24000, 2000, 22000, 20000}, .5);

    // The combinations of the curved cases agree with each other: |A| + |B| = |A or B| + |A and B|,
    // |A xor B| = |A or B| - |A and B| and |A - B| = |A or B| - |B|.
    for (u32 i = 0; i < kComboCount; ++i) {
        Case c;
        describe(shape_count + i * 4, &c);
        Primitive first, second;
        shape(c.first, &first);
        shape(c.second, &second);
        const Geometry* a = make(e.factory, first);
        const Geometry* moved = make(e.factory, second);
        const geometry::Matrix m{c.matrix[0], c.matrix[1], c.matrix[2], c.matrix[3], c.matrix[4], c.matrix[5]};
        const Geometry* b = moved ? geometry::geometry_transform(moved, m) : nullptr;
        double areas[4];
        for (u32 mode = 0; mode < 4; ++mode)
            areas[mode] = Exact::area(Exact::combine(a, b, mode));
        const double sa = Exact::area(a), sb = Exact::area(b);
        const double slack = 2 * kCombineTolerance * (Exact::outline(a) + Exact::outline(b)) + 1;
        e.expect("|A| + |B| = |A or B| + |A and B|", sa + sb - areas[0] - areas[1], 0, slack);
        e.expect("|A xor B| = |A or B| - |A and B|", areas[2] - (areas[0] - areas[1]), 0, slack);
        e.expect("|A - B| = |A or B| - |B|", areas[3] - (areas[0] - sb), 0, slack);
    }

    // Chains where each combination takes the last result, which the engine keeps rather than builds again, against
    // the same steps on copies it does not know: nine overlapping squares, a frame around them less them, strips cut
    // out of them and a band across, and six overlapping circles.
    const geometry::Matrix identity{1, 0, 0, 1, 0, 0};
    Geometry *chain = e.rect(0, 0, 50, 50), *copy = chain;
    for (u32 k = 1; k <= 8; ++k) {
        const float x = 40.f * static_cast<float>(k);
        chain = Exact::combine(chain, e.rect(x, 0, x + 50, 50), geometry::combine_union);
        copy = copy ? Exact::combine(geometry::geometry_transform(copy, identity), e.rect(x, 0, x + 50, 50),
                                     geometry::combine_union)
                    : nullptr;
    }
    e.expect("a chain of squares", Exact::area(chain), 18500, .5);
    e.expect("a copied chain of squares", Exact::area(copy), 18500, .5);
    e.expect("a frame less a chain",
             Exact::area(Exact::combine(e.rect(-10, -10, 400, 70), chain, geometry::combine_exclude)), 32800 - 18500,
             .5);
    for (u32 k = 0; k <= 8; ++k) {
        const float x = 40.f * static_cast<float>(k) + 20;
        chain = Exact::combine(chain, e.rect(x, -10, x + 10, 60), geometry::combine_exclude);
    }
    e.expect("a chain less strips", Exact::area(chain), 14000, .5);
    chain = Exact::combine(chain, e.rect(0, 20, 400, 30), geometry::combine_intersect);
    e.expect("a band of a chain less strips", Exact::area(chain), 2800, .5);
    Geometry *circles = e.circle(60, 60, 40), *copies = circles;
    for (u32 k = 1; k <= 5; ++k) {
        const float x = 60 + 50.f * static_cast<float>(k);
        circles = Exact::combine(circles, e.circle(x, 60, 40), geometry::combine_union);
        copies = copies ? Exact::combine(geometry::geometry_transform(copies, identity), e.circle(x, 60, 40),
                                         geometry::combine_union)
                        : nullptr;
    }
    // A kept result is the split outline it was made of; a copy is flattened again, its chords not quite the same.
    e.expect("a chain of circles", Exact::area(circles), Exact::area(copies),
             2 * kCombineTolerance * Exact::outline(circles) + 1);
    // Six discs of radius 40, 50 apart: each pair of neighbours overlaps by 3200 acos(5 / 8) - 25 sqrt(3900).
    const double lens = 3200 * 0.8956647939 - 25 * 62.4499799840;
    e.expect("a chain of circles drawn", Exact::area(circles), 6 * pi * 1600 - 5 * lens,
             2 * kCombineTolerance * Exact::outline(circles) + 1);

    // Strokes. Direct2D measured the corner (10,10) (60,10) (60,60) stroked 8 wide as 800, 846.848 and 856 square
    // units.
    const Point corner[]{{10, 10}, {60, 10}, {60, 60}};
    const Geometry* corner_path = e.polygon(corner, 3, false);
    const double corner_areas[]{800, 846.848, 856};
    const geometry::StrokeStyle styles[]{{8, 4, geometry::cap_flat, geometry::join_miter},
                                         {8, 4, geometry::cap_round, geometry::join_round},
                                         {8, 4, geometry::cap_square, geometry::join_bevel}};
    for (u32 i = 0; i < 3; ++i)
        e.expect("a stroked corner", Exact::area(Exact::stroke(corner_path, styles[i])), corner_areas[i], .6);
    // A path that turns back on itself, its round join half a disc past the turn; a square's outline stroked 10 wide,
    // drawn as a path that ends away from its start (closed by a line back to it) and as the rectangle primitive.
    const Point back[]{{10, 10}, {60, 10}, {10, 10}};
    e.expect("a stroke turning back",
             Exact::area(Exact::stroke(e.polygon(back, 3, false), {8, 4, geometry::cap_flat, geometry::join_round})),
             400 + 8 * pi, 1);
    const geometry::StrokeStyle wide{10, 4, geometry::cap_flat, geometry::join_miter};
    const Point outline[]{{0, 0}, {100, 0}, {100, 100}, {0, 100}};
    e.expect("a stroked square", Exact::area(Exact::stroke(e.polygon(outline, 4, true), wide)), 110 * 110 - 90 * 90,
             .5);
    e.expect("a stroked rectangle", Exact::area(Exact::stroke(square, wide)), 110 * 110 - 90 * 90, .5);
    // A zero-length open figure draws nothing, whatever its caps.
    const Point dot[]{{30, 30}};
    e.expect("a stroked point", Exact::area(Exact::stroke(e.polygon(dot, 1, false), styles[1])), 0, 0);
    geometry::factory_release(e.factory);
    return e.failures == 0;
}

bool run() noexcept {
    Arena arena{};
    if (!arena_initialize(&arena, usize{64} << 20, 0))
        return false;
    g_report = environment_flag("MAICORD_LOTTIE_REPORT");
    u32 failures = 0;
    test_check(&failures, exact_cases(), "the cases with exact answers");
    test_check(&failures, run_all(&arena, g_edges, g_engine), "the engine runs every case");
    const u32 stored = sizeof(kFixture) >= 4 ? *reinterpret_cast<const u32*>(kFixture) : 0;
    const bool fixture = sizeof(kFixture) == 4 + static_cast<usize>(kCaseCount) * kRecordBytes && stored == kCaseCount;
    test_check(&failures, fixture, "the fixture holds every case");
    for (u32 i = 0; i < kCaseCount && fixture; ++i) {
        Case c;
        describe(i, &c);
        Record reference;
        unpack(i, &reference);
        test_check(&failures, similar(i, c, reference, g_engine[i], 12), "the engine matches Direct2D");
    }
    return failures == 0;
}
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_lottie_geometry_entry) {
    maicord::test_exit(maicord::run() ? 0 : 1, "lottie geometry");
}
