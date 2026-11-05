#include "maicord/render/effects/vector_geometry.hpp"
#include "../../gpu/capture_io.hpp"
#include "lottie_geometry_raster.hpp"
#if defined(_WIN32)
#include "maicord/sys/d2d1.hpp"
#else
#include "maicord/os/linux/d2d1.hpp"
#endif

// The geometry the Lottie compiler asks Direct2D for, against the in-house engine that replaces it off Windows.
//
// A fixed list of cases (primitives, paths that cross themselves, the four boolean combinations of shapes that touch,
// coincide and are tangent, strokes of every cap, join and width) is run on the engine and compared with what Direct2D
// answered: the area of each result and its coverage on a grid, within the flattening tolerance. Windows runs Direct2D
// itself and the engine and compares them; with MAICORD_LOTTIE_GEOMETRY_CAPTURE=<file> it writes what Direct2D
// answered, and tests/render/effects/golden/lottie_geometry.bin (made that way) is what the engine is compared with
// everywhere else. Coordinates are in 512 units, the flattening tolerances are the ones the Lottie compiler passes at
// that size (0.1 for combines and strokes, 0.05 for areas).
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

// ---- backends: the same calls on Direct2D and on the engine ------------------------------------------------------

struct NativeBackend {
    using Factory = geometry::Factory;
    using Geometry = geometry::Geometry;
    using PathGeometry = geometry::Geometry;
    using EllipseGeometry = geometry::Geometry;
    using RoundedGeometry = geometry::Geometry;
    using RectangleGeometry = geometry::Geometry;
    using TransformedGeometry = geometry::Geometry;
    using GeometrySink = geometry::GeometrySink;
    using Sink = geometry::SimplifiedGeometrySink;
    using StrokeStyle = geometry::StrokeStyle;
    using Point = geometry::Point;
    using Rect = geometry::Rect;
    using Matrix = geometry::Matrix;
    using Bezier = geometry::BezierSegment;
    using Ellipse = geometry::Ellipse;
    using RoundedRect = geometry::RoundedRect;
    using StrokeProperties = geometry::StrokeStyleProperties;
    using FillMode = geometry::FillMode;
    using FigureBegin = geometry::FigureBegin;
    using FigureEnd = geometry::FigureEnd;
    using CombineMode = geometry::CombineMode;
    using CapStyle = geometry::CapStyle;
    using LineJoin = geometry::LineJoin;
    using Simplification = geometry::SimplificationOption;
    using PathSegment = geometry::PathSegment;
    struct SinkBase : geometry::SimplifiedGeometrySink {};
    static bool create(Factory** factory) noexcept { return geometry::factory_create(factory); }
    static void destroy(Factory* factory) noexcept { factory->Release(); }
    static constexpr bool ok(HRESULT result) noexcept { return result >= 0; }
};

#if defined(_WIN32)
struct D2dBackend {
    using Factory = ID2D1Factory;
    using Geometry = ID2D1Geometry;
    using PathGeometry = ID2D1PathGeometry;
    using EllipseGeometry = ID2D1EllipseGeometry;
    using RoundedGeometry = ID2D1RoundedRectangleGeometry;
    using RectangleGeometry = ID2D1RectangleGeometry;
    using TransformedGeometry = ID2D1TransformedGeometry;
    using GeometrySink = ID2D1GeometrySink;
    using Sink = ID2D1SimplifiedGeometrySink;
    using StrokeStyle = ID2D1StrokeStyle;
    using Point = D2D1_POINT_2F;
    using Rect = D2D1_RECT_F;
    using Matrix = D2D1_MATRIX_3X2_F;
    using Bezier = D2D1_BEZIER_SEGMENT;
    using Ellipse = D2D1_ELLIPSE;
    using RoundedRect = D2D1_ROUNDED_RECT;
    using StrokeProperties = D2D1_STROKE_STYLE_PROPERTIES;
    using FillMode = D2D1_FILL_MODE;
    using FigureBegin = D2D1_FIGURE_BEGIN;
    using FigureEnd = D2D1_FIGURE_END;
    using CombineMode = D2D1_COMBINE_MODE;
    using CapStyle = D2D1_CAP_STYLE;
    using LineJoin = D2D1_LINE_JOIN;
    using Simplification = D2D1_GEOMETRY_SIMPLIFICATION_OPTION;
    using PathSegment = D2D1_PATH_SEGMENT;
    // novtable: the base is abstract, and its own table would name _purecall, which the freestanding runtime lacks.
    struct __declspec(novtable) SinkBase : ID2D1SimplifiedGeometrySink {
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) noexcept override {
            if (!value)
                return E_POINTER;
            *value = nullptr;
            if (id == __uuidof(IUnknown) || id == __uuidof(ID2D1SimplifiedGeometrySink)) {
                *value = this;
                return S_OK;
            }
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1; }
        ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }
        void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) noexcept override {}
    };
    static bool create(Factory** factory) noexcept {
        return SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                                           reinterpret_cast<void**>(factory)));
    }
    static void destroy(Factory* factory) noexcept { factory->Release(); }
    static bool ok(HRESULT result) noexcept { return SUCCEEDED(result); }
};
#endif

// The edges, lengths and command counts of whatever a Simplify gives a sink.
template <typename B> struct Collector final : B::SinkBase {
    lottie_raster::Edges* edges;
    float tolerance;
    double length;
    u32 figures, commands, lines;
    bool even_odd;
    void STDMETHODCALLTYPE SetFillMode(typename B::FillMode mode) noexcept override {
        even_odd = static_cast<i32>(mode) == 0;
    }
    void STDMETHODCALLTYPE BeginFigure(typename B::Point p, typename B::FigureBegin) noexcept override {
        ++figures;
        ++commands;
        edges->begin(p.x, p.y);
    }
    void STDMETHODCALLTYPE AddLines(const typename B::Point* p, UINT32 n) noexcept override {
        for (UINT32 i = 0; i < n; ++i) {
            const double dx = p[i].x - edges->cx, dy = p[i].y - edges->cy;
            length += __builtin_elementwise_sqrt(dx * dx + dy * dy);
            edges->line(p[i].x, p[i].y);
            ++commands;
            ++lines;
        }
    }
    void STDMETHODCALLTYPE AddBeziers(const typename B::Bezier* b, UINT32 n) noexcept override {
        for (UINT32 i = 0; i < n; ++i) {
            const double dx = b[i].point3.x - edges->cx, dy = b[i].point3.y - edges->cy;
            length += __builtin_elementwise_sqrt(dx * dx + dy * dy);
            edges->cubic(b[i].point1.x, b[i].point1.y, b[i].point2.x, b[i].point2.y, b[i].point3.x, b[i].point3.y,
                         tolerance);
            ++commands;
        }
    }
    void STDMETHODCALLTYPE EndFigure(typename B::FigureEnd) noexcept override { edges->close(); }
    HRESULT STDMETHODCALLTYPE Close() noexcept override { return S_OK; }
};

struct Record {
    float area, length;
    u32 figures, commands;
    float bounds[4];
    u8 cells[kGrid * kGrid];
};
constexpr u32 kRecordBytes = 4 + 4 + 4 + 4 + 16 + kGrid * kGrid;

template <typename B> struct Scene {
    typename B::Factory* factory;
    Arena* arena;
    lottie_raster::Edge* edge_storage;

    typename B::Geometry* make(const Primitive& p) noexcept {
        typename B::Geometry* geometry = nullptr;
        switch (p.kind) {
        case Primitive::ellipse: {
            typename B::EllipseGeometry* g = nullptr;
            if (B::ok(factory->CreateEllipseGeometry(typename B::Ellipse{{p.a, p.b}, p.c, p.d}, &g)))
                geometry = g;
            break;
        }
        case Primitive::rounded: {
            typename B::RoundedGeometry* g = nullptr;
            if (B::ok(factory->CreateRoundedRectangleGeometry(
                    typename B::RoundedRect{{p.a, p.b, p.c, p.d}, p.e, p.f}, &g)))
                geometry = g;
            break;
        }
        case Primitive::rectangle: {
            typename B::RectangleGeometry* g = nullptr;
            const typename B::Rect r{p.a, p.b, p.c, p.d};
            if (B::ok(factory->CreateRectangleGeometry(&r, &g)))
                geometry = g;
            break;
        }
        case Primitive::path: {
            typename B::PathGeometry* g = nullptr;
            typename B::GeometrySink* sink = nullptr;
            if (!B::ok(factory->CreatePathGeometry(&g)) || !B::ok(g->Open(&sink)))
                break;
            sink->SetFillMode(static_cast<typename B::FillMode>(1));
            for (u32 f = 0; f < p.spec.figure_count; ++f) {
                const Spec::Figure& figure = p.spec.figures[f];
                sink->BeginFigure({figure.sx, figure.sy}, static_cast<typename B::FigureBegin>(0));
                for (u32 k = 0; k < figure.count; ++k) {
                    const float* q = p.spec.points[f][k];
                    const typename B::Bezier segment{{q[0], q[1]}, {q[2], q[3]}, {q[4], q[5]}};
                    sink->AddBezier(segment);
                }
                sink->EndFigure(static_cast<typename B::FigureEnd>(figure.closed ? 1 : 0));
            }
            const bool closed = B::ok(sink->Close());
            sink->Release();
            if (closed)
                geometry = g;
            break;
        }
        }
        return geometry;
    }

    // Measures `geometry` into a record: its area, bounds, size and coverage.
    bool measure(typename B::Geometry* geometry, Record* r) noexcept {
        float area = 0;
        typename B::Rect bounds{};
        if (!B::ok(geometry->ComputeArea(nullptr, kAreaTolerance, &area)) || !B::ok(geometry->GetBounds(nullptr, &bounds)))
            return false;
        lottie_raster::Edges edges{edge_storage, 0, 1 << 20, 0, 0, 0, 0};
        Collector<B> collector{};
        collector.edges = &edges;
        collector.tolerance = .02f;
        if (!B::ok(geometry->Simplify(static_cast<typename B::Simplification>(0), nullptr, kAreaTolerance, &collector)))
            return false;
        lottie_raster::Coverage<kGrid> coverage;
        if (!lottie_raster::rasterize<kGrid>(arena, edges, kExtent, collector.even_odd, &coverage))
            return false;
        r->area = area;
        r->length = static_cast<float>(collector.length);
        r->figures = collector.figures;
        r->commands = collector.commands;
        r->bounds[0] = bounds.left;
        r->bounds[1] = bounds.top;
        r->bounds[2] = bounds.right;
        r->bounds[3] = bounds.bottom;
        copy_bytes(r->cells, coverage.cells, sizeof(coverage.cells));
        return true;
    }

    // A combination or stroke of geometries into a path geometry.
    template <typename Fill> typename B::Geometry* build(Fill&& fill) noexcept {
        typename B::PathGeometry* g = nullptr;
        typename B::GeometrySink* sink = nullptr;
        if (!B::ok(factory->CreatePathGeometry(&g)) || !B::ok(g->Open(&sink)))
            return nullptr;
        const bool good = B::ok(fill(static_cast<typename B::Sink*>(sink)));
        const bool closed = B::ok(sink->Close());
        sink->Release();
        return good && closed ? g : nullptr;
    }

    bool run(const Case& c, Record* record) noexcept {
        Primitive first, second;
        shape(c.first, &first);
        typename B::Geometry* a = make(first);
        if (!a)
            return false;
        if (c.op == Op::shape)
            return measure(a, record);
        if (c.op == Op::widen) {
            typename B::StrokeProperties props{};
            props.startCap = props.endCap = props.dashCap = static_cast<typename B::CapStyle>(c.mode / 3);
            props.lineJoin = static_cast<typename B::LineJoin>(c.mode % 3);
            props.miterLimit = c.miter;
            typename B::StrokeStyle* style = nullptr;
            if (!B::ok(factory->CreateStrokeStyle(props, nullptr, 0, &style)))
                return false;
            typename B::Geometry* widened = build([&](typename B::Sink* sink) noexcept {
                return a->Widen(c.width, style, nullptr, kCombineTolerance, sink);
            });
            style->Release();
            return widened && measure(widened, record);
        }
        shape(c.second, &second);
        typename B::Geometry* b = make(second);
        if (!b)
            return false;
        // The second operand may be moved; the tangent pair is two circles of radius 100 instead.
        typename B::Matrix m{c.matrix[0], c.matrix[1], c.matrix[2], c.matrix[3], c.matrix[4], c.matrix[5]};
        typename B::TransformedGeometry* placed = nullptr;
        if (!B::ok(factory->CreateTransformedGeometry(b, &m, &placed)))
            return false;
        typename B::Geometry* combined = build([&](typename B::Sink* sink) noexcept {
            return a->CombineWithGeometry(placed, static_cast<typename B::CombineMode>(c.mode), nullptr,
                                          kCombineTolerance, sink);
        });
        return combined && measure(combined, record);
    }
};

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

template <typename B> bool run_all(Arena* arena, lottie_raster::Edge* storage, Record* out, u32* count) noexcept {
    typename B::Factory* factory = nullptr;
    if (!B::create(&factory))
        return false;
    Scene<B> scene{factory, arena, storage};
    bool ok = true;
    for (u32 i = 0; i < kCaseCount; ++i) {
        Case c;
        describe(i, &c);
        if (!scene.run(c, &out[i])) {
            print_case(i, c, "the backend failed");
            ok = false;
        }
    }
    *count = kCaseCount;
    B::destroy(factory);
    return ok;
}

Record g_native[kCaseCount], g_reference[kCaseCount];
lottie_raster::Edge g_edges[1 << 20];

bool unpack(u32 index, Record* r) noexcept {
    const u8* at = kFixture + 4 + static_cast<usize>(index) * kRecordBytes;
    copy_bytes(&r->area, at, 4);
    copy_bytes(&r->length, at + 4, 4);
    copy_bytes(&r->figures, at + 8, 4);
    copy_bytes(&r->commands, at + 12, 4);
    copy_bytes(r->bounds, at + 16, 16);
    copy_bytes(r->cells, at + 32, kGrid * kGrid);
    return true;
}

#if defined(_WIN32)
bool pack(Arena*, const Record* records, u32 count, const char* path) noexcept {
    static u8 bytes[4 + kCaseCount * kRecordBytes];
    copy_bytes(bytes, &count, 4);
    for (u32 i = 0; i < count; ++i) {
        u8* at = bytes + 4 + static_cast<usize>(i) * kRecordBytes;
        copy_bytes(at, &records[i].area, 4);
        copy_bytes(at + 4, &records[i].length, 4);
        copy_bytes(at + 8, &records[i].figures, 4);
        copy_bytes(at + 12, &records[i].commands, 4);
        copy_bytes(at + 16, records[i].bounds, 16);
        copy_bytes(at + 32, records[i].cells, kGrid * kGrid);
    }
    return capture_io::write_file(path, bytes, sizeof(bytes));
}
#endif

// Exact areas the engine must reproduce: shapes with a known area, and Direct2D's own answers for a stroked corner.
bool analytic_cases() noexcept {
    geometry::Factory* factory = nullptr;
    if (!geometry::factory_create(&factory))
        return false;
    u32 failures = 0;
    const auto near = [](double a, double b, double slack) noexcept { return (a < b ? b - a : a - b) <= slack; };
    const auto fail = [&failures](const char* what, u32 index) noexcept {
        ++failures;
        test_write_text("analytic case failed: ");
        test_write_metric(what, index);
    };
    // A path geometry holding one figure of the points (a corner, with its handles on its ends), or no figure.
    const auto path_of = [&](const geometry::Point* points, u32 count, geometry::Geometry** out) noexcept {
        geometry::GeometrySink* sink = nullptr;
        if (factory->CreatePathGeometry(out) < 0 || (*out)->Open(&sink) < 0)
            return false;
        if (count) {
            sink->BeginFigure(points[0], geometry::figure_filled);
            for (u32 i = 1; i < count; ++i)
                sink->AddBezier({points[i - 1], points[i], points[i]});
        }
        sink->EndFigure(geometry::figure_open);
        return sink->Close() >= 0;
    };
    // The same, widened into a new geometry.
    const auto widen = [&](geometry::Geometry* source, float width, geometry::CapStyle cap, geometry::LineJoin join,
                           geometry::Geometry** out) noexcept {
        geometry::StrokeStyle* style = nullptr;
        geometry::GeometrySink* sink = nullptr;
        return factory->CreateStrokeStyle({cap, cap, cap, join, 4, 0, 0}, nullptr, 0, &style) >= 0 &&
               factory->CreatePathGeometry(out) >= 0 && (*out)->Open(&sink) >= 0 &&
               source->Widen(width, style, nullptr, .1f, sink) >= 0 && sink->Close() >= 0;
    };
    geometry::Geometry *circle = nullptr, *first = nullptr, *second = nullptr;
    float area = 0;
    if (factory->CreateEllipseGeometry({{256, 256}, 100, 100}, &circle) < 0 || circle->ComputeArea(nullptr, .05f, &area) < 0 ||
        !near(area, 3.14159265 * 10000, 2000 * .05 * 3.14159265 + 1))
        fail("circle area x1000 ", static_cast<u32>(area * 1000));
    const geometry::Rect r{0, 0, 100, 100}, s{50, 50, 150, 150};
    if (factory->CreateRectangleGeometry(&r, &first) < 0 || factory->CreateRectangleGeometry(&s, &second) < 0) {
        fail("rectangles ", 0);
    } else {
        const double expected[]{17500, 2500, 15000, 7500};
        for (u32 mode = 0; mode < 4; ++mode) {
            geometry::Geometry* combined = nullptr;
            geometry::GeometrySink* sink = nullptr;
            float result = 0;
            if (factory->CreatePathGeometry(&combined) < 0 || combined->Open(&sink) < 0 ||
                first->CombineWithGeometry(second, static_cast<geometry::CombineMode>(mode), nullptr, .1f, sink) < 0 ||
                sink->Close() < 0 || combined->ComputeArea(nullptr, .05f, &result) < 0 || !near(result, expected[mode], .5))
                fail("rectangle combination, area x1000 ", static_cast<u32>(result * 1000));
        }
    }
    // D2D: the corner (10,10) (60,10) (60,60) stroked 8 wide measured 800, 846.848 and 856 square units.
    const geometry::Point corner[]{{10, 10}, {60, 10}, {60, 60}};
    geometry::Geometry* corner_path = nullptr;
    if (!path_of(corner, 3, &corner_path)) {
        fail("corner path ", 0);
    } else {
        const double expected[]{800, 846.848, 856};
        const geometry::CapStyle caps[]{geometry::cap_flat, geometry::cap_round, geometry::cap_square};
        const geometry::LineJoin joins[]{geometry::join_miter, geometry::join_round, geometry::join_bevel};
        for (u32 i = 0; i < 3; ++i) {
            geometry::Geometry* wide = nullptr;
            float stroke_area = 0;
            if (!widen(corner_path, 8, caps[i], joins[i], &wide) || wide->ComputeArea(nullptr, .05f, &stroke_area) < 0 ||
                !near(stroke_area, expected[i], .6))
                fail("stroked corner, area x1000 ", static_cast<u32>(stroke_area * 1000));
        }
    }
    // A zero-length open figure draws nothing, whatever its caps.
    const geometry::Point dot_point[]{{30, 30}};
    geometry::Geometry *dot = nullptr, *nothing = nullptr;
    geometry::Rect empty{};
    if (!path_of(dot_point, 1, &dot) || !widen(dot, 8, geometry::cap_round, geometry::join_round, &nothing) ||
        nothing->GetBounds(nullptr, &empty) < 0 || empty.left <= empty.right)
        fail("zero-length figure ", 0);
    factory->Release();
    return failures == 0;
}

bool run() noexcept {
    Arena arena{};
    if (!arena_initialize(&arena, usize{64} << 20, 0))
        return false;
    g_report = environment_flag("MAICORD_LOTTIE_REPORT");
    u32 failures = 0;
    test_check(&failures, analytic_cases(), "analytic geometry cases");
    u32 count = 0;
#if defined(_WIN32)
    char capture[520];
    if (capture_io::environment("MAICORD_LOTTIE_GEOMETRY_CAPTURE", capture, sizeof(capture))) {
        if (!run_all<D2dBackend>(&arena, g_edges, g_reference, &count) || !pack(&arena, g_reference, count, capture))
            return false;
        test_write_line("PASS captured the Direct2D answers");
        return failures == 0;
    }
#endif
    test_check(&failures, run_all<NativeBackend>(&arena, g_edges, g_native, &count), "the engine runs every case");
    const u32 stored = sizeof(kFixture) >= 4 ? *reinterpret_cast<const u32*>(kFixture) : 0;
    const bool fixture = sizeof(kFixture) == 4 + static_cast<usize>(kCaseCount) * kRecordBytes && stored == kCaseCount;
    test_check(&failures, fixture, "the fixture holds every case");
#if defined(_WIN32)
    // Direct2D answers again here, so the fixture is checked as well as the engine.
    u32 live = 0;
    test_check(&failures, run_all<D2dBackend>(&arena, g_edges, g_reference, &live), "Direct2D runs every case");
    for (u32 i = 0; i < kCaseCount && fixture; ++i) {
        Case c;
        describe(i, &c);
        Record stored_record;
        unpack(i, &stored_record);
        test_check(&failures, similar(i, c, stored_record, g_reference[i], 3), "the fixture matches Direct2D");
    }
#else
    for (u32 i = 0; i < kCaseCount && fixture; ++i)
        unpack(i, &g_reference[i]);
#endif
    if (fixture || sizeof(kFixture) > 1)
        for (u32 i = 0; i < kCaseCount; ++i) {
            Case c;
            describe(i, &c);
            test_check(&failures, similar(i, c, g_reference[i], g_native[i], 12), "the engine matches Direct2D");
        }
    return failures == 0;
}
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_lottie_geometry_entry) {
    maicord::test_exit(maicord::run() ? 0 : 1, "lottie geometry");
}
