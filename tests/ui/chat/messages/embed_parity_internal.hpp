#pragma once

#include "maicord/ui/text/measure_cache.hpp"
#include "maicord/ui/chat/messages/presentation.hpp"
#include "maicord/ui/chat/messages/text_coordinator.hpp"
#include "maicord/ui/text/native_text_candidate.hpp"

namespace maicord {
namespace embed_parity_detail {

using Check = void (*)(bool, const char*) noexcept;

extern TextSystem text_system;

extern wchar_t utf16[1024];

extern TextGlyph glyphs[1024];

extern TextRun runs[256];

extern u16 clusters[1024];

extern TextDecoration decorations[64];

// Simple scripts the native shaper draws from a covering system face: Bamum,
// Bamum Supplement, Yi, Vai, Tifinagh, Canadian Syllabics, N'Ko, Ol Chiki, Lisu.
constexpr u32 kSimpleScripts[]{0xA6A0, 0x169E7, 0xA000, 0xA564, 0x2D63, 0x1401, 0x07CA, 0x1C5A, 0xA4D0};

extern bool simple_script_covered[sizeof(kSimpleScripts) / sizeof(kSimpleScripts[0])];

bool close_to(float a, float b) noexcept;

bool ends_with(Utf8View text, Utf8View part) noexcept;

u32 encode(u32 cp, char* out) noexcept;

struct Shaped {
    bool ok;
    u32 notdef;
    u32 lines;
    float width;
    float advance;
    float height;
};

Shaped shape_styled(Utf8View text, ConstSlice<TextStyleSpan> styles = {}, float width = 1000) noexcept;

MessageMarkdownResult parse(Utf8View source, char* output, u32* styles, TextStyleSpan* spans,
                            MessageMarkdownSemantic* semantics, MessageMarkdownOptions options = {}) noexcept;

constexpr u32 kWidgets = 2 * kMessageSceneWidgetsPerRow + 1;
struct Fixture {
    MessageRecord record[2];
    u32 order[2];
    u8 text[2048];
    MessageRichRecord rich_record[2];
    u16 rich_order[2];
    u8 rich_text[32768];
    u32 heights[2];
    u64 fenwick[2];
    UiWidget widget[kWidgets];
    UiLayoutNode node[kWidgets];
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
    MessageScene scene;
    MessageTextCommand commands[256];
    char utf8[32768];
    TextStyleSpan styles[1024];
    MessageMarkdownSemantic semantics[256];
    MessageTextMeasurement measurements[4];
    MessageTextSpoilerReveal spoilers[4];
    u32 byte_styles[32768];
    MessageTextPresentation presentation;
    UiTextMeasureRecord cache_records[512];
    UiTextMeasureCache cache;
    PaintStream paint;
    MessageMediaCommand media[32];
};

extern Fixture fixture;

bool finish(void*, const Theme&) noexcept;

bool decode_message(Utf8View payload, GatewayMessageEvent* message) noexcept;

bool initialize_fixture(const GatewayMessageEvent& message, float height = 1000) noexcept;

const MessageTextCommand* command(const MessageTextPresentation& p, u16 id, u32 nth = 0) noexcept;

Utf8View link_target(const Fixture& f, const MessageTextCommand* c, u32 nth = 0) noexcept;

bool has_style(const MessageTextCommand* c, Utf8View part, u32 flags) noexcept;

u8 size_at(const MessageTextCommand* c, Utf8View part) noexcept;

bool sized_throughout(const MessageTextCommand* c, u8 size) noexcept;

float shaped_em(const MessageTextCommand* c, float* height = nullptr) noexcept;

void test_fxtwitter_embed(Check check) noexcept;

void test_thumbnail_rules(Check check) noexcept;

void test_gif_favorite_targets(Check check) noexcept;

void test_embed_gallery(Check check) noexcept;

void test_text_sizes(Check check) noexcept;

void test_quote_blocks(Check check) noexcept;

} // namespace embed_parity_detail

using namespace embed_parity_detail;

void test_native_text_never_fails(Check check) noexcept;

void test_vxtwitter_embeds(Check check) noexcept;

} // namespace maicord
