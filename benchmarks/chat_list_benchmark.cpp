// The chat timeline, head to head with BUI's variable-height list benchmark
// (blenderui-vlist-gpui examples/bench_list_gpui.rs): the same synthetic messages, the same 700x600 window and the
// same metrics, one JSON line per process. tools/benchmark_chat_list.ps1 runs it three times per size and writes the
// medians; the methodology, and where it differs from BUI's, is in tools/perf/README.md ("Chat list benchmark").
//
// What runs is the real thing: the message store, timeline and variable-height index (message_view_runtime), the
// message scene's layout over real shaped and wrapped text (message_text_coordinator_converge), the renderer's paint
// stream and glyph pipeline, and a real window and swap chain. A channel of n messages lives on the server: the
// resident window is kMessageViewItemCapacity messages, and everything else arrives the way history does in the
// client, as pages of kMessageHistoryRequestLimit messages in Discord's JSON, decoded by decode_message_history_page
// and applied by message_timeline_apply_history_page (evicting from the far edge). The harness is the server: it
// writes those JSON pages from the generator, with no network between (see tools/perf/README.md).
#include "maicord/app/chat/messages/history_sync.hpp"
#include "maicord/app/chat/messages/view_runtime.hpp"
#include "maicord/app/gateway/client_limits.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/model/chat/message_rich_store.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/gpu/surface.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/render/media/atlas.hpp"
#include "maicord/ui/chat/messages/text_coordinator.hpp"
#include "maicord/ui/core/widget_storage.hpp"
#include "maicord/ui/core/scene.hpp"
#include "maicord/ui/shell/navigation_view.hpp"
#include "maicord/ui/guild/rail_scene.hpp"
#include "maicord/ui/shell/navigation_scene.hpp"
#include "maicord/ui/text/measure_cache.hpp"

#if defined(_WIN32)
#include <Windows.h>
#include <psapi.h>
#include <intrin.h>

#include "../tools/perf/perf_imports.hpp"
#else
#include "maicord/core/intrinsics.hpp"
#include "maicord/platform/window/pump.hpp"
#include "maicord/platform/window/window.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/libc.hpp"
#include "maicord/sys/linux/process.hpp"
#endif

#if !defined(_WIN32)
#include "maicord/gpu/api.hpp"
#include "maicord/gpu/gles/backend.hpp"
#include "maicord/platform/window/wayland.hpp"
#endif
#include "maicord/core/log.hpp"
namespace maicord {
namespace {

// ---- the window, as BUI's harness has it ------------------------------------------------------------------------

inline constexpr u32 kTimelineWidthDips = 700;
inline constexpr u32 kTimelineHeightDips = 600;
// kGuildChannelShellLayoutMetrics: the server rail's width and the title bar's height around the timeline.
inline constexpr u32 kWindowWidthDips = kTimelineWidthDips + 64;
inline constexpr u32 kWindowHeightDips = kTimelineHeightDips + 48;
inline constexpr float kScrollDips = 57.0f;
inline constexpr u32 kJumps = 40;
inline constexpr u64 kJumpSeed = 0x9E37'79B9'7F4A'7C15ull;
inline constexpr u32 kMaximumSamples = 1u << 16;

// ---- the generator: blenderui-vlist/src/synth.rs, bit for bit --------------------------------------------------------

constexpr const char* kWords[24] = {"the",  "build",   "is",      "green", "again", "can",     "someone", "review",
                                    "my",   "patch",   "before",  "lunch", "ok",    "that",    "works",   "on",
                                    "windows", "but",  "not",     "linux", "thanks", "see",    "you",     "tomorrow"};
constexpr u32 kEpoch = 1'700'000'000u;
constexpr u32 kMaximumWords = 60 + 139;
constexpr u32 kMaximumTextBytes = kMaximumWords * 10;

constexpr u64 mix(u64 x) noexcept {
    x ^= x >> 33;
    x *= 0xff51'afd7'ed55'8ccdull;
    x ^= x >> 33;
    x *= 0xc4ce'b9fe'1a85'ec53ull;
    return x ^ (x >> 33);
}

struct Synth {
    u64 index;
    u32 time;
    u16 sender;
    u16 media_h;
    u32 text_bytes;
    char text[kMaximumTextBytes];
};

void synth_message(u64 i, Synth* m) noexcept {
    const u64 h = mix(i);
    m->index = i;
    m->sender = static_cast<u16>(h % 9);
    m->time = kEpoch + static_cast<u32>(i * 37) + static_cast<u32>(mix(i ^ 7) % 30);
    const u64 kind = (h >> 8) & 15;
    const u64 words = kind <= 9 ? 3 + (h >> 16) % 10 : kind <= 14 ? 14 + (h >> 16) % 30 : 60 + (h >> 16) % 140;
    u32 used = 0;
    u64 x = h;
    for (u64 k = 0; k < words; ++k) {
        x = mix(x + k);
        if (k > 0)
            m->text[used++] = ' ';
        const char* word = kWords[x % 24];
        for (u32 c = 0; word[c]; ++c)
            m->text[used++] = word[c];
    }
    m->text_bytes = used;
    m->media_h = (h >> 40) % 40 == 0 ? static_cast<u16>(120 + ((h >> 48) % 100)) : u16{0};
}

// Discord ids: the millisecond timestamp since 2015-01-01 above 22 bits. The message index is the low bits, so the
// ids ascend with the index (consecutive messages are 8 to 66 seconds apart).
constexpr u64 kDiscordEpochMs = 1'420'070'400'000ull;
constexpr u64 kAuthorBase = 1'250'000'000'000'000'000ull;
constexpr u64 kChannelId = 111;
constexpr u64 kGuildId = 100;

u32 message_time(u64 i) noexcept {
    return kEpoch + static_cast<u32>(i * 37) + static_cast<u32>(mix(i ^ 7) % 30);
}
u64 message_id(u64 i) noexcept {
    return ((static_cast<u64>(message_time(i)) * 1000ull - kDiscordEpochMs) << 22) | (i & 0x3F'FFFFull);
}

// ---- JSON writer for the pages and the result line ---------------------------------------------------------------

struct Text {
    char* data;
    u32 capacity;
    u32 used;
    bool overflow;

    void put(char c) noexcept {
        if (used < capacity)
            data[used++] = c;
        else
            overflow = true;
    }
    void put(const char* s) noexcept {
        while (*s)
            put(*s++);
    }
    void put(const char* s, u32 n) noexcept {
        for (u32 i = 0; i < n; ++i)
            put(s[i]);
    }
    void number(u64 v) noexcept {
        char digits[20];
        put(digits, decimal_write(digits, v));
    }
    void pad(u64 v, u32 width) noexcept {
        char digits[20];
        const u32 n = decimal_write(digits, v);
        for (u32 i = n; i < width; ++i)
            put('0');
        put(digits, n);
    }
    void fixed(double v, u32 decimals) noexcept {
        if (v < 0) {
            put('-');
            v = -v;
        }
        u64 scale = 1;
        for (u32 i = 0; i < decimals; ++i)
            scale *= 10;
        const u64 scaled = static_cast<u64>(v * static_cast<double>(scale) + 0.5);
        number(scaled / scale);
        if (decimals) {
            put('.');
            pad(scaled % scale, decimals);
        }
    }
};

// Days since 1970-01-01 to a civil date (Hinnant).
void civil(i64 days, i64* year, u32* month, u32* day) noexcept {
    const i64 z = days + 719'468;
    const i64 era = (z >= 0 ? z : z - 146'096) / 146'097;
    const i64 doe = z - era * 146'097;
    const i64 yoe = (doe - doe / 1460 + doe / 36'524 - doe / 146'096) / 365;
    const i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const i64 mp = (5 * doy + 2) / 153;
    *day = static_cast<u32>(doy - (153 * mp + 2) / 5 + 1);
    *month = static_cast<u32>(mp < 10 ? mp + 3 : mp - 9);
    *year = yoe + era * 400 + (*month <= 2 ? 1 : 0);
}

// One message the way GET /channels/{id}/messages sends it.
void write_message(Text* out, const Synth& m) noexcept {
    i64 year = 0;
    u32 month = 0, day = 0;
    civil(m.time / 86'400, &year, &month, &day);
    const u32 seconds = m.time % 86'400;
    out->put("{\"id\":\"");
    out->number(message_id(m.index));
    out->put("\",\"type\":0,\"channel_id\":\"");
    out->number(kChannelId);
    out->put("\",\"author\":{\"id\":\"");
    out->number(kAuthorBase + 1 + m.sender);
    out->put("\",\"username\":\"user");
    out->number(m.sender);
    out->put("\",\"global_name\":\"User ");
    out->number(m.sender);
    out->put("\",\"avatar\":null,\"discriminator\":\"0\",\"public_flags\":0},\"content\":\"");
    out->put(m.text, m.text_bytes);
    out->put("\",\"mentions\":[],\"mention_roles\":[],\"pinned\":false,\"mention_everyone\":false,\"tts\":false,"
             "\"timestamp\":\"");
    out->number(static_cast<u64>(year));
    out->put('-');
    out->pad(month, 2);
    out->put('-');
    out->pad(day, 2);
    out->put('T');
    out->pad(seconds / 3600, 2);
    out->put(':');
    out->pad(seconds / 60 % 60, 2);
    out->put(':');
    out->pad(seconds % 60, 2);
    out->put(".000000+00:00\",\"edited_timestamp\":null,\"flags\":0,\"components\":[],\"attachments\":[],"
             "\"embeds\":[]}");
}

// ---- measurement ------------------------------------------------------------------------------------------------

struct Samples {
    u32* us;
    u32 count;

    void clear() noexcept { count = 0; }
    void add(u64 value) noexcept {
        if (count < kMaximumSamples)
            us[count++] = static_cast<u32>(value > 0xFFFF'FFFFull ? 0xFFFF'FFFFull : value);
    }
    // Ascending in place (heap sort: the harness has no library sort).
    void sort() noexcept {
        const auto sift = [&](u32 root, u32 end) noexcept {
            for (;;) {
                u32 child = root * 2 + 1;
                if (child >= end)
                    return;
                if (child + 1 < end && us[child] < us[child + 1])
                    ++child;
                if (us[root] >= us[child])
                    return;
                const u32 t = us[root];
                us[root] = us[child];
                us[child] = t;
                root = child;
            }
        };
        for (u32 i = count / 2; i-- > 0;)
            sift(i, count);
        for (u32 end = count; end > 1;) {
            --end;
            const u32 t = us[0];
            us[0] = us[end];
            us[end] = t;
            sift(0, end);
        }
    }
    // Of a sorted array: BUI's pct(): sorted[round((n - 1) * p)].
    double percentile(double p) const noexcept {
        if (!count)
            return 0.0;
        const double position = static_cast<double>(count - 1) * p;
        const u32 index = static_cast<u32>(position + 0.5);
        return static_cast<double>(us[index]) / 1000.0;
    }
    double mean() const noexcept {
        if (!count)
            return 0.0;
        double sum = 0;
        for (u32 i = 0; i < count; ++i)
            sum += static_cast<double>(us[i]);
        return sum / static_cast<double>(count) / 1000.0;
    }
    double maximum_ms() const noexcept { return count ? static_cast<double>(us[count - 1]) / 1000.0 : 0.0; }
};

struct StageStats {
    u32 frames;
    u32 moved;
    double fps, mean, p50, p95, p99, max, cpu_ms, allocs;
    double layout_p50, text_p50, paint_p50, text_p95, text_p99, paint_p99;
};

// ---- the allocations of this image (heap blocks and pages) on the UI thread -----------------------------------

#if defined(_WIN32)
using HeapAllocFn = LPVOID(WINAPI*)(HANDLE, DWORD, SIZE_T);
using HeapReAllocFn = LPVOID(WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);
using VirtualAllocFn = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
HeapAllocFn g_heap_alloc = nullptr;
HeapReAllocFn g_heap_realloc = nullptr;
VirtualAllocFn g_virtual_alloc = nullptr;
#endif
volatile u64 g_ui_allocs = 0;
#if defined(_WIN32)
DWORD g_ui_thread = 0;
#else
u32 g_ui_thread = 0;
#endif
// Ticks the harness spends playing the server (writing a page's JSON): not the client's work, so every timing and the
// CPU figure leave it out.
i64 g_server_ticks = 0;
i64 g_decode_ticks = 0, g_apply_ticks = 0;

#if defined(_WIN32)
LPVOID WINAPI hook_heap_alloc(HANDLE heap, DWORD flags, SIZE_T bytes) noexcept {
    if (GetCurrentThreadId() == g_ui_thread)
        g_ui_allocs = g_ui_allocs + 1;
    return g_heap_alloc(heap, flags, bytes);
}
LPVOID WINAPI hook_heap_realloc(HANDLE heap, DWORD flags, LPVOID block, SIZE_T bytes) noexcept {
    if (GetCurrentThreadId() == g_ui_thread)
        g_ui_allocs = g_ui_allocs + 1;
    return g_heap_realloc(heap, flags, block, bytes);
}
LPVOID WINAPI hook_virtual_alloc(LPVOID address, SIZE_T bytes, DWORD type, DWORD protect) noexcept {
    if (GetCurrentThreadId() == g_ui_thread)
        g_ui_allocs = g_ui_allocs + 1;
    return g_virtual_alloc(address, bytes, type, protect);
}

u32 install_allocation_counters() noexcept {
    auto* base = reinterpret_cast<u8*>(GetModuleHandleW(nullptr));
    struct Hook {
        const char* name;
        void* replacement;
        void** original;
    } hooks[] = {
        {"HeapAlloc", reinterpret_cast<void*>(hook_heap_alloc), reinterpret_cast<void**>(&g_heap_alloc)},
        {"HeapReAlloc", reinterpret_cast<void*>(hook_heap_realloc), reinterpret_cast<void**>(&g_heap_realloc)},
        {"VirtualAlloc", reinterpret_cast<void*>(hook_virtual_alloc), reinterpret_cast<void**>(&g_virtual_alloc)},
    };
    u32 installed = 0;
    perf::for_each_import_slot(
        base, 3, [&](u32 i) noexcept { return hooks[i].name; }, false,
        [&](u32 i, IMAGE_THUNK_DATA64* slot) noexcept {
            DWORD old = 0;
            if (!VirtualProtect(&slot->u1.Function, sizeof(u64), PAGE_READWRITE, &old))
                return false;
            *hooks[i].original = reinterpret_cast<void*>(slot->u1.Function);
            slot->u1.Function = reinterpret_cast<u64>(hooks[i].replacement);
            VirtualProtect(&slot->u1.Function, sizeof(u64), old, &old);
            ++installed;
            return true;
        });
    return installed;
}
#else
// The image's own calls of the C library's heap (heap_allocate is malloc and calloc there): the program links with
// --wrap for them, so every call of the image comes through these (see below). Page allocations (the arenas) are
// raw syscalls on Linux and are not counted.
u32 install_allocation_counters() noexcept {
    return 3;
}
#endif

// ---- the fixture: the client's own owners for one channel's timeline, with no shell around it -----------------

inline constexpr u32 kPageMessages = 25; // kMessageHistoryRequestLimit of the client (account_ui_runtime.hpp)
inline constexpr u32 kMessageSceneRowCapacity = 192; // app/shell/state.hpp
inline constexpr u32 kSceneWidgetCapacity = kMessageSceneRowCapacity * kMessageSceneWidgetsPerRow + 1;
inline constexpr u32 kPageStructurals = 64 * 1024;
inline constexpr u32 kBodyBytes = 192 * 1024;
inline constexpr u32 kUserCapacity = 64;
inline constexpr u32 kRichTextBytes = 256 * 1024;
inline constexpr u16 kMediaExtent = 256;
inline constexpr u32 kMediaEntries = 32;
inline constexpr u32 kMediaRows = 16;
inline constexpr u32 kNameBytes = 16 * 1024;
inline constexpr u32 kGuildCapacity = 4;
inline constexpr u32 kChannelCapacity = 16;

struct Fixture {
    MessageViewRuntime view;
    D3D11Renderer renderer;
    UiScene shell_scene;
    MessageScene scene;
    GuildRailScene guild_rail_scene;
    GuildRailRowWidget guild_rail_rows[kGuildCapacity];
    UiWidget guild_rail_widgets[kGuildCapacity + 3 + 2 * kGuildRailPluginTiles];
    UiLayoutNode guild_rail_nodes[kGuildCapacity + 3 + 2 * kGuildRailPluginTiles];
    ShellNavigationScene navigation_scene;
    ShellNavigationRowWidget navigation_rows[kChannelCapacity];
    UiWidget navigation_widgets[kChannelCapacity * 4 + 2];
    UiLayoutNode navigation_nodes[kChannelCapacity * 4 + 2];
    UiWidgetStorageOwner scene_storage;
    ChatStateStore chat_state;
    UserStore users;
    NavigationView navigation;
    MessageTextPresentation presentation;
    UiTextMeasureCache measure;
    RendererTextStorage text;
    MessageRichStore rich;
    MediaAtlas atlas, preview_atlas;
    // storage of the owners above
    GuildRecord guilds[kGuildCapacity];
    ChannelRecord channels[kChannelCapacity];
    u16 guild_order[kGuildCapacity];
    u16 channel_order[kChannelCapacity];
    u8 names[kNameBytes];
    UserRecord user_records[kUserCapacity];
    u32 user_order[kUserCapacity];
    u8 user_names[16 * 1024];
    GuildViewRow guild_rows[kGuildCapacity];
    ChannelViewRow channel_rows[kChannelCapacity];
    u16 channel_scratch[kChannelCapacity];
    u8 channel_visibility_scratch[kChannelCapacity];
    ChannelSortKey channel_sort_keys[kChannelCapacity];
    MessageRichRecord rich_records[kMessageViewItemCapacity];
    u16 rich_order[kMessageViewItemCapacity];
    u8 rich_text[kRichTextBytes];
    MediaAtlasEntry media_entries[kMediaEntries];
    u32 media_buckets[kMediaEntries * 2];
    GlyphAtlasEntry media_allocations[kMediaEntries];
    GlyphAtlasRow media_rows[kMediaRows];
    MediaAtlasEntry preview_entries[kMediaEntries];
    u32 preview_buckets[kMediaEntries * 2];
    GlyphAtlasEntry preview_allocations[kMediaEntries];
    GlyphAtlasRow preview_rows[kMediaRows];
    // text pipeline
    GlyphQuad text_quads[kRendererContentGlyphCapacity];
    GlyphDrawBatch text_batches[kRendererContentBatchCapacity];
    wchar_t shape_utf16[kRendererContentTextUnitCapacity];
    TextGlyph shape_glyphs[kRendererContentTextUnitCapacity];
    TextRun shape_runs[kRendererContentRunCapacity];
    u16 shape_clusters[kRendererContentTextUnitCapacity];
    MessageTextCommand message_text_commands[kMaximumMessageTextCommands];
    char message_text_utf8[kMaximumMessageTextUtf8Bytes];
    TextStyleSpan message_text_styles[kMaximumMessageTextStyleSpans];
    MessageMarkdownSemantic message_text_semantics[kMaximumMessageTextSemantics];
    MessageTextMeasurement message_text_measurements[kMaximumMessageTextMeasurements];
    UiTextMeasureRecord measure_records[kMaximumMessageTextMeasureCacheRecords];
    MessageTextSpoilerReveal spoiler_reveals[kMaximumMessageTextSpoilerReveals];
    u32 byte_styles[kMessageTextByteStyleScratchCapacity];
    TextHitSpan hit_spans[kTextHitSpanMaximum];
    TextHitRegion hit_region_scratch[kRendererContentHitRegionCapacity];
    TextDecoration text_decorations[kRendererTextDecorationCapacity];
    MessageTextSemantic content_semantics[kRendererContentSemanticCapacity];
    TextHitRegion content_hit_regions[kRendererContentHitRegionCapacity];
    // history pages: the body, the structural scratch and the decode bank of one page
    char body[kBodyBytes];
    u32 page_structurals[kPageStructurals];
    char decoded_content[kGatewayClientDispatchPayloadCapacity];
    GatewayMessageEvent page_messages[kPageMessages];
    GatewayMessageNamedUser named_users[kPageMessages * kGatewayMessageMentionUserMaximum];
    u64 member_roles[kPageMessages * 100];
    GatewayMessageReaction reactions[kPageMessages * kGatewayMessageReactionMaximum];
    GatewayMessageAttachment attachments[kPageMessages * kGatewayMessageAttachmentMaximum];
    GatewayMessageEmbed embeds[kPageMessages * kGatewayMessageEmbedMaximum];
    GatewayMessageEmbedField embed_fields[kPageMessages * kGatewayMessageEmbedFieldMaximum];
    GatewayMessageSticker stickers[kPageMessages * kGatewayMessageStickerMaximum];
    GatewayMessageComponent components[kPageMessages * kGatewayMessageComponentMaximum];
    Synth synth;
    u32 sample_a[kMaximumSamples];
    u32 sample_b[kMaximumSamples];
    u32 sample_c[kMaximumSamples];
    u32 sample_d[kMaximumSamples];
    u32 sample_e[kMaximumSamples];
};

Fixture* g_f = nullptr;
#if defined(_WIN32)
HWND g_window = nullptr;
#else
Window g_window = nullptr;
#endif
bool g_closed = false;

struct Bench {
    u64 count;           // messages in the channel
    u64 newest_resident; // index of the newest message in the window
    u64 oldest_resident;
    u32 pages;           // history pages applied
    u32 evicted;
    u32 jumps_fetched;
    bool exhausted_newer;
    u32 failure;         // nonzero: where the harness stopped
    u32 scene_width_dips, scene_height_dips;
};
Bench g_b{};
Theme g_theme;

// ---- the model: history pages, the way the client applies them ---------------------------------------------------

enum class Page : u8 { latest, newer, around };

bool apply_page(Page kind, u64 anchor) noexcept {
    Fixture& f = *g_f;
    // The query: which messages the server returns, newest first.
    u64 from = 0, to = 0;
    switch (kind) {
    case Page::latest:
        from = g_b.count > kPageMessages ? g_b.count - kPageMessages : 0;
        to = g_b.count;
        break;
    case Page::newer:
        from = anchor + 1;
        to = from + kPageMessages < g_b.count ? from + kPageMessages : g_b.count;
        break;
    case Page::around:
        from = anchor > kPageMessages / 2 ? anchor - kPageMessages / 2 : 0;
        to = anchor + kPageMessages / 2 + 1 < g_b.count ? anchor + kPageMessages / 2 + 1 : g_b.count;
        break;
    }
    if (from >= to)
        return false;
    const i64 server_begin = clock_now();
    Text body{f.body, kBodyBytes, 0, false};
    body.put('[');
    for (u64 i = to; i-- > from;) {
        synth_message(i, &f.synth);
        if (i + 1 != to)
            body.put(',');
        write_message(&body, f.synth);
    }
    body.put(']');
    if (body.overflow)
        return false;
    g_server_ticks += clock_now() - server_begin;
    const MessageHistoryRichScratch rich{
        {f.reactions, kPageMessages * kGatewayMessageReactionMaximum},
        {f.attachments, kPageMessages * kGatewayMessageAttachmentMaximum},
        {f.embeds, kPageMessages * kGatewayMessageEmbedMaximum},
        {f.embed_fields, kPageMessages * kGatewayMessageEmbedFieldMaximum},
        {f.stickers, kPageMessages * kGatewayMessageStickerMaximum},
        {f.components, kPageMessages * kGatewayMessageComponentMaximum},
        {f.member_roles, kPageMessages * 100},
        {},
        {}};
    const i64 decode_begin = clock_now();
    const MessageHistoryDecodeResult decoded = decode_message_history_page(
        Utf8View{f.body, body.used}, Slice<u32>{f.page_structurals, kPageStructurals}, 64,
        Slice<char>{f.decoded_content, kGatewayClientDispatchPayloadCapacity},
        Slice<GatewayMessageEvent>{f.page_messages, kPageMessages},
        Slice<GatewayMessageNamedUser>{f.named_users, kPageMessages * kGatewayMessageMentionUserMaximum}, rich);
    g_decode_ticks += clock_now() - decode_begin;
    if (!decoded.ok() || decoded.page.count != to - from)
        return false;
    const ConstSlice<GatewayMessageEvent> page{f.page_messages, decoded.page.count};
    if (!message_history_observe_page(&f.users, nullptr, nullptr, page))
        return false;
    MessageTimeline& timeline = f.view.timeline;
    if (kind == Page::around && !message_timeline_reset(&timeline))
        return false;
    const i64 apply_begin = clock_now();
    const MessageHistoryApplyResult applied = message_timeline_apply_history_page(
        &timeline, page, MessageHistoryDirection::newer, MessageContentAvailability::unknown);
    g_apply_ticks += clock_now() - apply_begin;
    if (!applied.ok())
        return false;
    ++g_b.pages;
    g_b.evicted += applied.evicted;
    if (kind == Page::around) {
        g_b.oldest_resident = from;
        g_b.newest_resident = to - 1;
    } else if (kind == Page::latest) {
        g_b.oldest_resident = from;
        g_b.newest_resident = to - 1;
    } else {
        g_b.newest_resident = to - 1;
        // The window holds the newest kMessageViewItemCapacity messages up to there.
        if (g_b.newest_resident + 1 - g_b.oldest_resident > kMessageViewItemCapacity)
            g_b.oldest_resident = g_b.newest_resident + 1 - kMessageViewItemCapacity;
    }
    if (kind == Page::around) {
        // The edges the page proves (the client's own function), and the paging state they leave.
        const MessageHistoryJumpEdges edges =
            message_history_jump_edges(page, message_id(anchor), kPageMessages, message_id(g_b.count - 1));
        timeline.newer_refetch_required = edges.more_after;
        timeline.follow_bottom = false;
        g_b.exhausted_newer = !edges.more_after;
    } else if (kind == Page::newer) {
        g_b.exhausted_newer = g_b.newest_resident + 1 >= g_b.count;
        timeline.newer_refetch_required = !g_b.exhausted_newer;
    } else {
        g_b.exhausted_newer = true;
        timeline.newer_refetch_required = false;
    }
    return true;
}

// Scrolls the timeline so message `index` (resident) tops the viewport, as the jump to a message does.
bool scroll_to_resident(u64 index) noexcept {
    Fixture& f = *g_f;
    MessageTimeline& timeline = f.view.timeline;
    const MessageStoreResult found = message_store_find(f.view.store, message_id(index));
    UiVariableList* const list = timeline.list;
    if (!found.ok() || found.order_index >= list->count)
        return false;
    const u64 top = ui_variable_list_prefix_units(*list, found.order_index);
    const u64 maximum = message_timeline_maximum_scroll(timeline);
    return message_timeline_set_scroll(&timeline, top < maximum ? top : maximum, false);
}

bool resident(u64 index) noexcept {
    return message_store_find(g_f->view.store, message_id(index)).ok();
}

// ---- one frame -------------------------------------------------------------------------------------------------

struct FrameTimes {
    i64 begin, laid_out, shaped, painted, server0;
};

bool converge(bool scroll_refresh, bool scroll_moving) noexcept {
    Fixture& f = *g_f;
    const MessageTextCoordinatorRequest request{
        &f.scene,
        &f.presentation,
        &f.measure,
        &f.chat_state,
        &f.users,
        &f.navigation,
        &g_theme,
        +[](void* owner, const Theme& theme) noexcept {
            return renderer_rebuild_content_text(static_cast<D3D11Renderer*>(owner), theme);
        },
        &f.renderer,
        1'700'000'000ull,
        renderer_text_measure_revision(f.renderer),
        nullptr,
        scroll_refresh,
        scroll_moving};
    const MessageTextCoordinatorResult result = message_text_coordinator_converge(request);
    if (!result.ok())
        g_b.failure = 400 + static_cast<u32>(result.error) * 10 + static_cast<u32>(result.scene_error) +
                      1000 * static_cast<u32>(f.text.last_error) + 100'000 * (f.renderer.glyph_text.ready ? 1 : 0) +
                      200'000 * (f.presentation.active ? 1 : 0) + 400'000 * (f.renderer.content_text ? 1 : 0);
    return result.ok();
}

UiRect message_viewport() noexcept {
    return ui_scene_content_rectangle(g_f->shell_scene, UiSemantic::message_view);
}

#if defined(_WIN32)
void pump_window() noexcept {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT)
            g_closed = true;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
#else
bool pump_before(void*) noexcept {
    return true;
}
bool pump_prefilter(void*, const WindowEvent&) noexcept {
    return false;
}
void pump_after(void*) noexcept {
}
void pump_window() noexcept {
    const WindowPumpHooks hooks{pump_before, pump_prefilter, pump_after, nullptr};
    if (window_pump(64, 2'000, hooks).quit)
        g_closed = true;
}
#endif

// Waits for the swap chain's slot the way the client's loop does before it samples input, so the frame's own time
// excludes the wait for the display.
bool acquire_frame() noexcept {
    pump_window();
    D3D11Renderer& r = g_f->renderer;
    if (gpu_frame_clock_waitable(&r.graphics.surface) && !r.begin_frame_permit) {
        if (!gpu_frame_wait(&r.graphics.surface, 1'000u))
            return false;
        renderer_accept_begin_frame(&r);
    }
    return true;
}

// The layout, text and paint of the state as it stands (after a jump or the first page).
bool full_frame(FrameTimes* times) noexcept {
    Fixture& f = *g_f;
    times->begin = clock_now();
    times->server0 = g_server_ticks;
    const MessageSceneResult updated = message_scene_update(&f.scene, message_viewport());
    if (!updated.ok()) {
        g_b.failure = 300 + static_cast<u32>(updated.error);
        return false;
    }
    times->laid_out = clock_now();
    if (!converge(false, false))
        return false;
    times->shaped = clock_now();
    if (!renderer_present(&f.renderer, g_theme)) {
        g_b.failure = 399;
        return false;
    }
    times->painted = clock_now();
    return true;
}

// Moves the timeline by `dips` and draws it: a smooth-scroll tick. `moved` is false when the timeline is at its
// resident end (no frame was drawn). Without `shortcut` the text is projected again instead of being moved.
bool scroll_frame(FrameTimes* times, bool* moved, bool shortcut) noexcept {
    Fixture& f = *g_f;
    MessageTimeline& timeline = f.view.timeline;
    times->begin = clock_now();
    times->server0 = g_server_ticks;
    bool changed = false;
    if (!message_scene_scroll_by(&f.scene, static_cast<i64>(ui_extent_units(kScrollDips)), ui_extent_units(1.0f),
                                 &changed))
        return false;
    // Near the resident bottom of a window that is not the live edge: the next page (the client submits the request
    // here and applies the response when it arrives; with no network that is this frame).
    if (message_history_newer_prefetch_needed(timeline)) {
        if (!apply_page(Page::newer, g_b.newest_resident))
            return false;
        changed = true;
    }
    *moved = changed;
    times->laid_out = clock_now();
    if (!changed) {
        times->shaped = times->painted = times->laid_out;
        return true;
    }
    if (!converge(shortcut, shortcut))
        return false;
    times->shaped = clock_now();
    if (!renderer_present(&f.renderer, g_theme))
        return false;
    times->painted = clock_now();
    return true;
}

// A forced repaint of the unchanged state: BUI rebuilds and repaints every frame it is asked for; here the retained
// presentation is dropped so the paint stream is built again (the layout and the shaped text stay as they are).
bool still_frame(FrameTimes* times) noexcept {
    Fixture& f = *g_f;
    times->begin = clock_now();
    times->server0 = g_server_ticks;
    f.renderer.retained_presentation.valid = false;
    presentation_damage_invalidate_full(&f.renderer.presentation_damage);
    times->laid_out = times->shaped = clock_now();
    if (!renderer_present(&f.renderer, g_theme))
        return false;
    times->painted = clock_now();
    return true;
}

// A jump to message `index`: the page around it when it is not resident, then the first frame there.
bool jump_to(u64 index, FrameTimes* times) noexcept {
    Fixture& f = *g_f;
    times->begin = clock_now();
    times->server0 = g_server_ticks;
    if (!resident(index)) {
        if (!apply_page(Page::around, index))
            return false;
        ++g_b.jumps_fetched;
    }
    if (!message_scene_update(&f.scene, message_viewport()).ok() || !scroll_to_resident(index) ||
        !message_scene_update(&f.scene, message_viewport()).ok())
        return false;
    times->laid_out = clock_now();
    if (!converge(false, false))
        return false;
    times->shaped = clock_now();
    if (!renderer_present(&f.renderer, g_theme))
        return false;
    times->painted = clock_now();
    return true;
}

// ---- process figures --------------------------------------------------------------------------------------------

struct Memory {
    u64 working_set, peak_working_set, private_bytes;
};
#if defined(_WIN32)
Memory memory() noexcept {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
    return {counters.WorkingSetSize, counters.PeakWorkingSetSize, counters.PrivateUsage};
}
u64 process_cycles() noexcept {
    ULONG64 cycles = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &cycles);
    return cycles;
}
double tsc_hz(const Clock& clock) noexcept {
    const i64 t0 = clock_now();
    const u64 c0 = __rdtsc();
    Sleep(300);
    const i64 t1 = clock_now();
    const u64 c1 = __rdtsc();
    return static_cast<double>(c1 - c0) * 1e6 / static_cast<double>(clock_microseconds(clock, t0, t1));
}
#else
// Linux: the cycle counter is the process's CPU time in nanoseconds, so a "rate" of 1e9 turns it into seconds.
Memory memory() noexcept {
    Memory result{};
    const long fd = sys_open("/proc/self/status", 0);
    if (fd < 0)
        return result;
    char text[4096];
    const long n = sys_read(static_cast<int>(fd), text, sizeof(text) - 1);
    sys_close(static_cast<int>(fd));
    if (n <= 0)
        return result;
    text[n] = 0;
    const auto value = [&](const char* key) noexcept -> u64 {
        for (long i = 0; i < n; ++i) {
            u32 k = 0;
            while (key[k] && text[i + k] == key[k])
                ++k;
            if (key[k])
                continue;
            u64 kib = 0;
            for (long j = i + k; j < n && text[j] != '\n'; ++j)
                if (text[j] >= '0' && text[j] <= '9')
                    kib = kib * 10 + static_cast<u64>(text[j] - '0');
            return kib * 1024;
        }
        return 0;
    };
    result.working_set = value("VmRSS:");
    result.peak_working_set = value("VmHWM:");
    result.private_bytes = value("RssAnon:");
    return result;
}
u64 process_cycles() noexcept {
    TimeSpec time{};
    clock_gettime(2, &time); // CLOCK_PROCESS_CPUTIME_ID
    return static_cast<u64>(time.seconds) * 1'000'000'000ull + static_cast<u64>(time.nanoseconds);
}
double tsc_hz(const Clock&) noexcept {
    return 1e9;
}
#endif
double mb(u64 bytes) noexcept {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

#if defined(_WIN32)
// ---- an in-process sampler of the UI thread's instruction pointer (--profile) -------------------------------------
// No unwinding (the image has no unwind tables): each sample is the leaf, counted per address in a hash table; the
// result lines `P <module> <rva> <count>` are resolved against the linker map by tools/perf/profile_chat_list.py.
// The suspend and resume of each sample perturb the timing, so a profiled run's frame figures are not reported.

struct ProfileSlot {
    u64 address;
    u32 count;
};
inline constexpr u32 kProfileSlots = 1u << 16;
ProfileSlot* g_profile = nullptr;
volatile u32 g_profile_running = 0;
u32 g_profile_total = 0, g_profile_dropped = 0;
HANDLE g_profile_target = nullptr;
void write_stdout(const char* data, u32 bytes) noexcept;

u32 profile_thread(void*) noexcept {
    const Clock clock = clock_initialize();
    while (g_profile_running) {
        // A spin, not Sleep: the timer's granularity is 15 ms. The sampling core is busy for the profiled run.
        const i64 mark = clock_now();
        while (clock_microseconds(clock, mark, clock_now()) < 200)
            YieldProcessor();
        if (SuspendThread(g_profile_target) == static_cast<DWORD>(-1))
            continue;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        const bool got = GetThreadContext(g_profile_target, &context) != 0;
        ResumeThread(g_profile_target);
        if (!got)
            continue;
        const u64 address = context.Rip;
        u32 slot = static_cast<u32>((address * 0x9E37'79B9'7F4A'7C15ull) >> 48);
        u32 tries = 0;
        while (g_profile[slot].address && g_profile[slot].address != address && tries < 64) {
            slot = (slot + 1) & (kProfileSlots - 1);
            ++tries;
        }
        ++g_profile_total;
        if (g_profile[slot].address && g_profile[slot].address != address) {
            ++g_profile_dropped;
            continue;
        }
        g_profile[slot].address = address;
        ++g_profile[slot].count;
    }
    return 0;
}

void profiler_start() noexcept {
    g_profile = static_cast<ProfileSlot*>(pages_allocate(sizeof(ProfileSlot) * kProfileSlots));
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_profile_target, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    g_profile_running = 1;
    thread_start(profile_thread, nullptr, 0, nullptr);
}

void profiler_stop() noexcept {
    g_profile_running = 0;
    Sleep(20);
}

void write_profile() noexcept {
    char line[160];
    Text header{line, sizeof(line), 0, false};
    header.put("P total ");
    header.number(g_profile_total);
    header.put(" dropped ");
    header.number(g_profile_dropped);
    header.put('\n');
    write_stdout(line, header.used);
    for (u32 slot = 0; slot < kProfileSlots; ++slot) {
        if (!g_profile[slot].address)
            continue;
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(g_profile[slot].address), &module);
        wchar_t name[64]{};
        if (module)
            GetModuleFileNameW(module, name, 63);
        u32 leaf = 0;
        for (u32 i = 0; name[i]; ++i)
            if (name[i] == L'\\')
                leaf = i + 1;
        Text out{line, sizeof(line), 0, false};
        out.put("P ");
        if (!name[leaf])
            out.put("?");
        for (u32 i = leaf; name[i]; ++i)
            out.put(static_cast<char>(name[i]));
        out.put(' ');
        out.number(g_profile[slot].address - reinterpret_cast<u64>(module));
        out.put(' ');
        out.number(g_profile[slot].count);
        out.put('\n');
        write_stdout(line, out.used);
    }
}

#endif
#if !defined(_WIN32)
void profiler_start() noexcept {
}
void profiler_stop() noexcept {
}
void write_profile() noexcept {
}
#endif

// ---- the watchdog: a cap on memory and time, ending the process with a line that says how far it got -----------

struct Watchdog {
    u64 base_working_set;
    double cap_mb, limit_s;
    u64 count;
    i64 start;
    Clock clock;
};
Watchdog g_watchdog{};
volatile u32 g_stage = 0;
constexpr const char* kStages[6] = {"setup", "first frame", "scroll", "still", "jump", "done"};
#if defined(_WIN32)
void write_stdout(const char* data, u32 bytes) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), data, bytes, &written, nullptr);
}
[[noreturn]] void exit_process(int code) noexcept {
    ExitProcess(static_cast<UINT>(code));
}
#else
void write_stdout(const char* data, u32 bytes) noexcept {
    for (u32 done = 0; done < bytes;) {
        const long n = sys_write(1, data + done, bytes - done);
        if (n <= 0)
            return;
        done += static_cast<u32>(n);
    }
}
[[noreturn]] void exit_process(int code) noexcept {
    sys_exit_group(code);
}
#endif

u32 watchdog_thread(void*) noexcept {
    for (;;) {
        thread_sleep(100);
        const double over = mb(memory().working_set) - mb(g_watchdog.base_working_set);
        const double elapsed =
            static_cast<double>(clock_microseconds(g_watchdog.clock, g_watchdog.start, clock_now())) / 1e6;
        const char* why = over > g_watchdog.cap_mb ? "memory cap" : elapsed > g_watchdog.limit_s ? "time limit" : nullptr;
        if (!why)
            continue;
        char line[512];
        Text out{line, sizeof(line), 0, false};
        out.put("{\"lib\":\"MaiCord timeline\",\"n\":");
        out.number(g_watchdog.count);
        out.put(",\"failed\":\"");
        out.put(why);
        out.put("\",\"stage\":\"");
        out.put(kStages[g_stage > 5 ? 5 : g_stage]);
        out.put("\",\"elapsed_s\":");
        out.fixed(elapsed, 1);
        out.put(",\"ram_above_data_mb\":");
        out.fixed(over, 0);
        out.put(",\"cap_mb\":");
        out.fixed(g_watchdog.cap_mb, 0);
        out.put(",\"limit_s\":");
        out.fixed(g_watchdog.limit_s, 0);
        out.put("}\n");
        write_stdout(line, out.used);
        exit_process(3);
    }
}

// ---- arguments ----------------------------------------------------------------------------------------------------

struct Args {
    u64 count;
    double seconds, warmup, cap_mb, limit_s;
    bool profile;
};

template <class C> bool text_equals(const C* a, const char* b) noexcept {
    while (*a && *a == static_cast<C>(*b)) {
        ++a;
        ++b;
    }
    return *a == static_cast<C>(*b);
}
template <class C> double parse_number(const C* s) noexcept {
    double value = 0, scale = 0;
    for (; *s; ++s) {
        if (*s == '.')
            scale = 1;
        else if (*s >= '0' && *s <= '9') {
            value = value * 10 + (*s - '0');
            scale *= 10;
        }
    }
    return scale ? value / scale : value;
}
// "--name value" pairs after the program's name.
template <class C> void apply_arguments(Args* a, C* const* token, u32 count) noexcept {
    for (u32 i = 1; i + 1 < count; ++i) {
        if (text_equals(token[i], "--n"))
            a->count = static_cast<u64>(parse_number(token[i + 1]));
        else if (text_equals(token[i], "--seconds"))
            a->seconds = parse_number(token[i + 1]);
        else if (text_equals(token[i], "--warmup"))
            a->warmup = parse_number(token[i + 1]);
        else if (text_equals(token[i], "--cap-mb"))
            a->cap_mb = parse_number(token[i + 1]);
        else if (text_equals(token[i], "--limit-seconds"))
            a->limit_s = parse_number(token[i + 1]);
        else if (text_equals(token[i], "--profile"))
            a->profile = parse_number(token[i + 1]) != 0;
    }
}
Args parse_arguments() noexcept {
    Args a{100000, 6.0, 2.0, 4096.0, 150.0};
#if defined(_WIN32)
    wchar_t* cursor = GetCommandLineW();
    wchar_t* token[16]{};
    u32 count = 0;
    while (*cursor && count < 16) {
        while (*cursor == L' ')
            ++cursor;
        if (!*cursor)
            break;
        token[count++] = cursor;
        while (*cursor && *cursor != L' ')
            ++cursor;
        if (*cursor)
            *cursor++ = 0;
    }
    apply_arguments(&a, token, count);
#else
    const ProcessStartup& startup = process_startup();
    apply_arguments(&a, startup.argv, static_cast<u32>(startup.argc));
#endif
    return a;
}

// ---- setup: window, renderer, the channel's timeline -------------------------------------------------------------

#if defined(_WIN32)
LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    switch (message) {
    case WM_CLOSE:
        g_closed = true;
        return 0;
    case WM_PAINT:
        ValidateRect(window, nullptr);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

bool create_window(GpuSurfaceDesc* surface) noexcept {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = window_procedure;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = L"MaiCordChatListBenchmark";
    if (!RegisterClassExW(&type))
        return false;
    const u32 dpi = GetDpiForSystem();
    const LONG width = static_cast<LONG>(kWindowWidthDips * dpi / 96), height = static_cast<LONG>(kWindowHeightDips * dpi / 96);
    RECT bounds{0, 0, width, height};
    // WS_EX_NOREDIRECTIONBITMAP: the composition swap chain the client presents through.
    constexpr DWORD kExtended = WS_EX_NOREDIRECTIONBITMAP;
    constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    AdjustWindowRectExForDpi(&bounds, kStyle, FALSE, kExtended, dpi);
    g_window = CreateWindowExW(kExtended, type.lpszClassName, L"MaiCord chat list benchmark", kStyle, 64, 64,
                               bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, instance,
                               nullptr);
    if (!g_window)
        return false;
    ShowWindow(g_window, SW_SHOW);
    SetForegroundWindow(g_window);
    UpdateWindow(g_window);
    RECT client{};
    GetClientRect(g_window, &client);
    const u32 actual = GetDpiForWindow(g_window);
    *surface = GpuSurfaceDesc{g_window, static_cast<u32>(client.right - client.left),
                              static_cast<u32>(client.bottom - client.top), actual ? actual : 96};
    return true;
}
#else
// Wayland: the window service the client uses; the compositor (headless sway, or WSLg's) decides the frame pacing.
void window_sink(void*, const WindowEvent&, WindowEventResult*) noexcept {
}

bool create_window(GpuSurfaceDesc* surface) noexcept {
    // As the client's window glue: Mesa's d3d12 driver on WSL, and the GPU device made on the compositor's connection.
    gpu_device_prefer_wsl_gpu();
    if (void* display = window_wayland_connect())
        gpu_device_set_native_display(display);
    const WindowCreateInfo info{utf8_literal("MaiCord chat list benchmark"),
                                utf8_literal("io.github.maj113.MaiCord.ChatListBenchmark"), kWindowWidthDips,
                                kWindowHeightDips, true};
    if (!window_create(info, window_sink, nullptr, &g_window))
        return false;
    window_show(g_window, ShowMode::show);
    // The compositor sizes the window with its first configure: serve events until the client area has a size.
    for (u32 i = 0; i < 200 && window_metrics(g_window).width_px == 0; ++i) {
        pump_window();
        thread_sleep(10);
    }
    *surface = renderer_window_surface(g_window);
    return true;
}
#endif

bool add_guild(u64 id, Utf8View name) noexcept {
    return chat_state_store_upsert_guild(&g_f->chat_state, GuildInput{id, name, GuildMutationKind::create}).ok();
}
bool add_channel(u64 id, u64 guild_id, u8 type, Utf8View name) noexcept {
    return chat_state_store_upsert_channel(&g_f->chat_state, ChannelInput{id, guild_id, 0, name, 0, 0, type}).ok();
}

// Zero, or the number of the step that failed.
u32 initialize_models() noexcept {
    Fixture& f = *g_f;
    if (!chat_state_store_initialize(&f.chat_state, Slice<GuildRecord>{f.guilds, kGuildCapacity},
                                     Slice<u16>{f.guild_order, kGuildCapacity},
                                     Slice<ChannelRecord>{f.channels, kChannelCapacity},
                                     Slice<u16>{f.channel_order, kChannelCapacity}, Slice<u8>{f.names, kNameBytes}))
        return 1;
    if (!user_store_initialize(&f.users, Slice<UserRecord>{f.user_records, kUserCapacity},
                               Slice<u32>{f.user_order, kUserCapacity}, Slice<u8>{f.user_names, sizeof(f.user_names)}))
        return 2;
    if (!message_rich_store_initialize(&f.rich, Slice<MessageRichRecord>{f.rich_records, kMessageViewItemCapacity},
                                       Slice<u16>{f.rich_order, kMessageViewItemCapacity},
                                       Slice<u8>{f.rich_text, kRichTextBytes}))
        return 3;
    if (!add_guild(kGuildId, utf8_literal("Benchmark")))
        return 4;
    if (!add_channel(kChannelId, kGuildId, 0, utf8_literal("general")))
        return 5;
    if (!chat_state_store_note_message(&f.chat_state, kChannelId, message_id(g_b.count - 1), kAuthorBase + 1, false, false))
        return 6;
    if (!chat_state_store_select_channel(&f.chat_state, kChannelId).ok())
        return 7;
    if (!navigation_view_initialize(&f.navigation, Slice<GuildViewRow>{f.guild_rows, kGuildCapacity},
                                    Slice<ChannelViewRow>{f.channel_rows, kChannelCapacity},
                                    Slice<u16>{f.channel_scratch, kChannelCapacity},
                                    Slice<u8>{f.channel_visibility_scratch, kChannelCapacity},
                                    Slice<ChannelSortKey>{f.channel_sort_keys, kChannelCapacity}))
        return 8;
    if (!navigation_view_rebuild(&f.navigation, f.chat_state).ok())
        return 9;
    return 0;
}

bool initialize_timeline() noexcept {
    Fixture& f = *g_f;
    // The client's runtime: the store, the variable-height index and the timeline over them, following the bottom.
    if (!message_view_runtime_initialize(&f.view, ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height), 1,
                                         true)
             .ok() ||
        !message_view_runtime_bind_rich_store(&f.view, &f.rich).ok())
        return false;
    if (!ui_widget_storage_initialize(&f.scene_storage, kSceneWidgetCapacity, 1) ||
        !message_scene_initialize_owned(&f.scene, &f.view.timeline, kMessageSceneRowCapacity, 32,
                                        ui_widget_storage_widgets(f.scene_storage),
                                        ui_widget_storage_layout_nodes(f.scene_storage), true) ||
        !ui_widget_manager_bind_growable_storage(&f.scene.widgets, ui_widget_storage_grow, &f.scene_storage))
        return false;
    f.scene.bottom_anchored = true;
    return true;
}

bool initialize_renderer_bindings(const GpuSurfaceDesc& surface) noexcept {
    Fixture& f = *g_f;
    if (!renderer_initialize(&f.renderer, surface, true)) {
        g_b.failure = 100;
        return false;
    }
    f.text.quads = f.text_quads;
    f.text.batches = f.text_batches;
    f.text.utf16 = f.shape_utf16;
    f.text.glyphs = f.shape_glyphs;
    f.text.runs = f.shape_runs;
    f.text.clusters = f.shape_clusters;
    f.text.hit_spans = f.hit_spans;
    f.text.hit_region_scratch = f.hit_region_scratch;
    f.text.decorations = f.text_decorations;
    f.text.content_semantics = f.content_semantics;
    f.text.content_hit_regions = f.content_hit_regions;
    f.text.quad_capacity = kRendererContentGlyphCapacity;
    f.text.batch_capacity = kRendererContentBatchCapacity;
    f.text.utf16_capacity = kRendererContentTextUnitCapacity;
    f.text.glyph_capacity = kRendererContentTextUnitCapacity;
    f.text.run_capacity = kRendererContentRunCapacity;
    f.text.cluster_capacity = kRendererContentTextUnitCapacity;
    f.text.hit_span_capacity = kTextHitSpanMaximum;
    f.text.hit_region_scratch_capacity = kRendererContentHitRegionCapacity;
    f.text.decoration_capacity = kRendererTextDecorationCapacity;
    f.text.content_semantic_capacity = kRendererContentSemanticCapacity;
    f.text.content_hit_region_capacity = kRendererContentHitRegionCapacity;
    renderer_bind_navigation_view(&f.renderer, &f.chat_state, &f.users, &f.navigation);
    const auto step = [](bool ok, u32 id) noexcept {
        if (!ok && !g_b.failure)
            g_b.failure = id;
        return ok;
    };
    if (!step(media_atlas_initialize(&f.atlas, kMediaExtent, kMediaExtent, 1,
                                     Slice<MediaAtlasEntry>{f.media_entries, kMediaEntries},
                                     Slice<u32>{f.media_buckets, kMediaEntries * 2},
                                     Slice<GlyphAtlasEntry>{f.media_allocations, kMediaEntries},
                                     Slice<GlyphAtlasRow>{f.media_rows, kMediaRows}),
              1) ||
        !step(media_atlas_initialize(&f.preview_atlas, kMediaExtent, kMediaExtent, 1,
                                     Slice<MediaAtlasEntry>{f.preview_entries, kMediaEntries},
                                     Slice<u32>{f.preview_buckets, kMediaEntries * 2},
                                     Slice<GlyphAtlasEntry>{f.preview_allocations, kMediaEntries},
                                     Slice<GlyphAtlasRow>{f.preview_rows, kMediaRows}),
              2) ||
        !step(renderer_bind_media_atlas(&f.renderer, &f.atlas), 3) ||
        !step(renderer_bind_preview_media_atlas(&f.renderer, &f.preview_atlas), 4) ||
        !step(renderer_bind_text_storage(&f.renderer, &f.text), 5) ||
        !step(message_text_presentation_initialize(
                  &f.presentation, Slice<MessageTextCommand>{f.message_text_commands, kMaximumMessageTextCommands},
                  Slice<char>{f.message_text_utf8, kMaximumMessageTextUtf8Bytes},
                  Slice<TextStyleSpan>{f.message_text_styles, kMaximumMessageTextStyleSpans},
                  Slice<MessageMarkdownSemantic>{f.message_text_semantics, kMaximumMessageTextSemantics},
                  Slice<MessageTextMeasurement>{f.message_text_measurements, kMaximumMessageTextMeasurements},
                  Slice<MessageTextSpoilerReveal>{f.spoiler_reveals, kMaximumMessageTextSpoilerReveals},
                  Slice<u32>{f.byte_styles, kMessageTextByteStyleScratchCapacity}),
              6) ||
        !step(ui_text_measure_cache_initialize(
                  &f.measure, Slice<UiTextMeasureRecord>{f.measure_records, kMaximumMessageTextMeasureCacheRecords},
                  renderer_text_measure_backend, &f.renderer),
              7) ||
        !step(renderer_bind_message_text_presentation(&f.renderer, &f.presentation), 8))
        return false;
    f.renderer.message_measure_cache = &f.measure;
    return true;
}

// The shell keeps only what it cannot lay out without (the server rail's column and the title bar's row): the
// window is that much larger than the 700x600 timeline, so the timeline is exactly BUI's list size.
bool layout_shell() noexcept {
    Fixture& f = *g_f;
    UiShellLayoutMetrics metrics = kGuildChannelShellLayoutMetrics;
    metrics.flags = 0;
    metrics.navigation_width = 0;
    metrics.member_sidebar_width = 0;
    metrics.composer_height = 0;
    metrics.message_bottom_margin = 0;
    metrics.composer_margin = UiInsets{};
    metrics.revision = 7001;
    const float scale = static_cast<float>(maximum<u32>(f.renderer.dpi, 1)) / 96.0f;
    const float width = static_cast<float>(f.renderer.width) / scale, height = static_cast<float>(f.renderer.height) / scale;
    if (!ui_scene_initialize_bootstrap(&f.shell_scene)) {
        g_b.failure = 201;
        return false;
    }
    if (!ui_scene_layout_with_metrics(&f.shell_scene, UiShellView::guild_channel, metrics, width, height)) {
        g_b.failure = 202;
        return false;
    }
    if (!renderer_bind_ui_scene(&f.renderer, &f.shell_scene)) {
        g_b.failure = 203;
        return false;
    }
    if (!renderer_bind_message_scene(&f.renderer, &f.scene)) {
        g_b.failure = 204;
        return false;
    }
    // The renderer lays the shell's text out only with the rail and channel list bound; both stay empty here
    // (no navigation column, no servers shown).
    if (!guild_rail_scene_initialize(&f.guild_rail_scene, Slice<GuildRailRowWidget>{f.guild_rail_rows, kGuildCapacity},
                                     Slice<UiWidget>{f.guild_rail_widgets, kGuildCapacity + 3 + 2 * kGuildRailPluginTiles},
                                     Slice<UiLayoutNode>{f.guild_rail_nodes, kGuildCapacity + 3 + 2 * kGuildRailPluginTiles}) ||
        !shell_navigation_scene_initialize(&f.navigation_scene,
                                           Slice<ShellNavigationRowWidget>{f.navigation_rows, kChannelCapacity},
                                           Slice<UiWidget>{f.navigation_widgets, kChannelCapacity * 4 + 2},
                                           Slice<UiLayoutNode>{f.navigation_nodes, kChannelCapacity * 4 + 2})) {
        g_b.failure = 205;
        return false;
    }
    const UiLayoutNode* rail = ui_scene_node(f.shell_scene, UiSemantic::guild_navigation);
    const UiLayoutNode* channels = ui_scene_node(f.shell_scene, UiSemantic::channel_navigation);
    if (!guild_rail_scene_update(&f.guild_rail_scene, &f.navigation, rail ? rail->rectangle : UiRect{}, 0.0f).ok() ||
        !shell_navigation_scene_update(&f.navigation_scene, &f.navigation, channels ? channels->rectangle : UiRect{},
                                       0.0f)
             .ok() ||
        !renderer_bind_guild_rail_scene(&f.renderer, &f.guild_rail_scene) ||
        !renderer_bind_shell_navigation_scene(&f.renderer, &f.navigation_scene)) {
        g_b.failure = 206;
        return false;
    }
    const UiRect viewport = message_viewport();
    g_b.scene_width_dips = static_cast<u32>(ui_rect_width(viewport) + 0.5f);
    g_b.scene_height_dips = static_cast<u32>(ui_rect_height(viewport) + 0.5f);
    f.scene.pixel_scale = scale;
    return true;
}

// ---- stages ---------------------------------------------------------------------------------------------------------

// scroll_full: the same scroll without the scroll-only shortcut (the text projection is rebuilt every frame, the way a
// resize or a new page converges); the figure to set beside a library that rebuilds its frame each time.
enum class Mode : u8 { scroll, scroll_full, still };

// Runs frames for `warm` seconds (not recorded), then `seconds` (recorded).
bool run_stage(Mode mode, double warm, double seconds, double hz, StageStats* out) noexcept {
    Fixture& f = *g_f;
    const Clock clock = clock_initialize();
    Samples frame{f.sample_a, 0}, layout{f.sample_b, 0}, text{f.sample_c, 0}, paint{f.sample_d, 0};
    const i64 begin = clock_now();
    const u64 warm_us = static_cast<u64>(warm * 1e6), total_us = static_cast<u64>((warm + seconds) * 1e6);
    bool recording = false;
    u64 cycles0 = 0, allocs0 = 0, frames = 0, moved_frames = 0;
    i64 server_ticks0 = 0;
    i64 record_begin = 0;
    for (;;) {
        const u64 elapsed = clock_microseconds(clock, begin, clock_now());
        if (elapsed >= total_us)
            break;
        if (!recording && elapsed >= warm_us) {
            recording = true;
            record_begin = clock_now();
            cycles0 = process_cycles();
            server_ticks0 = g_server_ticks;
            allocs0 = g_ui_allocs;
        }
        if (g_closed || !acquire_frame())
            return false;
        FrameTimes t{};
        bool moved = true;
        if (mode == Mode::scroll || mode == Mode::scroll_full) {
            if (!scroll_frame(&t, &moved, mode == Mode::scroll))
                return false;
            if (!moved) {
                // The live edge of the channel (or the end of what is resident with nothing newer): BUI's list
                // keeps repainting its clamped position; this one has nothing to draw, so the run starts over from
                // the middle (not a recorded frame).
                FrameTimes jump{};
                if (!jump_to(g_b.count / 2, &jump))
                    return false;
                continue;
            }
        } else if (!still_frame(&t)) {
            return false;
        }
        if (recording) {
            ++frames;
            moved_frames += moved ? 1 : 0;
            const u64 total_us = clock_microseconds(clock, t.begin, t.painted);
            const u64 server_us = clock_microseconds(clock, 0, g_server_ticks - t.server0);
            const u64 layout_us = clock_microseconds(clock, t.begin, t.laid_out);
            frame.add(total_us - server_us);
            layout.add(layout_us > server_us ? layout_us - server_us : 0);
            text.add(clock_microseconds(clock, t.laid_out, t.shaped));
            paint.add(clock_microseconds(clock, t.shaped, t.painted));
        }
    }
    const u64 wall_us = clock_microseconds(clock, record_begin, clock_now());
    const u64 server_total_us = clock_microseconds(clock, 0, g_server_ticks - server_ticks0);
    const double cpu_server_ms = static_cast<double>(server_total_us) / 1000.0;
    const u64 cycles = process_cycles() - cycles0;
    const u64 allocs = g_ui_allocs - allocs0;
    frame.sort();
    layout.sort();
    text.sort();
    paint.sort();
    const double n = frames ? static_cast<double>(frames) : 1.0;
    out->frames = static_cast<u32>(frames);
    out->moved = static_cast<u32>(moved_frames);
    out->fps = wall_us ? static_cast<double>(frames) * 1e6 / static_cast<double>(wall_us) : 0.0;
    out->mean = frame.mean();
    out->p50 = frame.percentile(0.5);
    out->p95 = frame.percentile(0.95);
    out->p99 = frame.percentile(0.99);
    out->max = frame.maximum_ms();
    out->cpu_ms = (static_cast<double>(cycles) / hz * 1000.0 - cpu_server_ms) / n;
    out->allocs = static_cast<double>(allocs) / n;
    out->layout_p50 = layout.percentile(0.5);
    out->text_p50 = text.percentile(0.5);
    out->paint_p50 = paint.percentile(0.5);
    out->text_p95 = text.percentile(0.95);
    out->text_p99 = text.percentile(0.99);
    out->paint_p99 = paint.percentile(0.99);
    return true;
}

int run() noexcept {
    const Clock clock = clock_initialize();
    const i64 t_start = clock_now();
    const Args args = parse_arguments();
#if !defined(_WIN32)
    (void)log_open(); // renderer and window failures are only in the log
#endif
    g_b.count = args.count;
#if defined(_WIN32)
    g_ui_thread = GetCurrentThreadId();
#else
    g_ui_thread = current_thread_id();
#endif
    const u32 hooks = install_allocation_counters();
    // No data to build: the channel is a generator behind the history pages.
    const Memory base = memory();
    g_watchdog = {base.working_set, args.cap_mb, args.limit_s, args.count, t_start, clock};
    thread_start(watchdog_thread, nullptr, 0, nullptr);

    auto* f = static_cast<Fixture*>(pages_allocate(sizeof(Fixture)));
    if (!f)
        return 10;
    g_f = f;
    f->sample_a[0] = 0;
    g_theme = kDefaultTheme;
    f->scene.pixel_scale = 1.0f;
    GpuSurfaceDesc surface{};
    if (!create_window(&surface))
        return 11;
    if (const u32 step = initialize_models())
        return 100 + static_cast<int>(step);
    // setup: the timeline's owners and the scene over them (BUI: the list's state for n items).
    const i64 setup_begin = clock_now();
    if (!initialize_timeline())
        return 13;
    const u64 setup_us = clock_microseconds(clock, setup_begin, clock_now());
    if (!initialize_renderer_bindings(surface) || !layout_shell())
        return 1000 + static_cast<int>(g_b.failure);
    g_stage = 1;
    // The first frame: the newest page, laid out, shaped and painted.
    if (!apply_page(Page::latest, 0))
        return 15;
    FrameTimes first{};
    if (!full_frame(&first))
        return 2000 + static_cast<int>(g_b.failure);
    const double first_frame_ms = static_cast<double>(clock_microseconds(clock, t_start, first.painted)) / 1000.0;
    const double hz = tsc_hz(clock);
    // BUI waits 300 ms here, then opens the middle of the list.
    thread_sleep(300);
    if (g_b.count < 2 || !acquire_frame())
        return 17;
    FrameTimes middle{};
    if (!jump_to(g_b.count / 2, &middle))
        return 18;
    g_stage = 2;
    StageStats scroll{}, still{};
    if (args.profile)
        profiler_start();
    const bool scrolled = run_stage(Mode::scroll, args.warmup, args.seconds, hz, &scroll);
    if (args.profile)
        profiler_stop();
    if (!scrolled)
        return 4000 + static_cast<int>(g_b.failure);
    const Memory after = memory();
    g_stage = 3;
    const double still_seconds = args.seconds / 2.0 > 2.0 ? args.seconds / 2.0 : 2.0;
    if (!run_stage(Mode::still, 1.0, still_seconds, hz, &still))
        return 2000 + static_cast<int>(g_b.failure);
    StageStats full{};
    if (!run_stage(Mode::scroll_full, args.warmup, args.seconds / 2.0, hz, &full))
        return 3000 + static_cast<int>(g_b.failure);
    g_stage = 4;
    Samples jumps{f->sample_e, 0};
    u64 x = kJumpSeed;
    for (u32 i = 0; i < kJumps; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        const u64 item = x % g_b.count;
        if (!acquire_frame())
            return 21;
        FrameTimes t{};
        if (!jump_to(item, &t))
            return 22;
        jumps.add(clock_microseconds(clock, t.begin, t.painted) -
                  clock_microseconds(clock, 0, g_server_ticks - t.server0));
        thread_sleep(30);
    }
    jumps.sort();
    g_stage = 5;
    const Memory end = memory();

    char line[2048];
    Text out{line, sizeof(line), 0, false};
    const auto field = [&](const char* key, double value, u32 decimals, bool first_field = false) noexcept {
        if (!first_field)
            out.put(',');
        out.put('"');
        out.put(key);
        out.put("\":");
        out.fixed(value, decimals);
    };
    out.put("{\"lib\":\"MaiCord timeline\",\"n\":");
    out.number(g_b.count);
    field("data_ms", 0, 0);
    field("setup_ms", static_cast<double>(setup_us) / 1000.0, 3);
    field("first_frame_ms", first_frame_ms, 1);
    field("scroll_frames", scroll.frames, 0);
    field("scroll_moved_frames", scroll.moved, 0);
    field("fps", scroll.fps, 1);
    field("frame_ms_mean", scroll.mean, 3);
    field("frame_ms_p50", scroll.p50, 3);
    field("frame_ms_p95", scroll.p95, 3);
    field("frame_ms_p99", scroll.p99, 3);
    field("frame_ms_max", scroll.max, 3);
    field("cpu_ms_per_frame", scroll.cpu_ms, 3);
    field("main_allocs_per_frame", scroll.allocs, 1);
    field("layout_ms_p50", scroll.layout_p50, 3);
    field("text_ms_p50", scroll.text_p50, 3);
    field("paint_ms_p50", scroll.paint_p50, 3);
    field("scroll_full_frame_ms_p50", full.p50, 3);
    field("scroll_full_frame_ms_p95", full.p95, 3);
    field("scroll_full_frame_ms_p99", full.p99, 3);
    field("scroll_full_cpu_ms_per_frame", full.cpu_ms, 3);
    field("scroll_full_main_allocs_per_frame", full.allocs, 1);
    field("scroll_full_text_ms_p50", full.text_p50, 3);
    field("text_ms_p95", scroll.text_p95, 3);
    field("text_ms_p99", scroll.text_p99, 3);
    field("paint_ms_p99", scroll.paint_p99, 3);
    field("page_decode_ms_mean", g_b.pages ? static_cast<double>(clock_microseconds(clock, 0, g_decode_ticks)) /
                                                 1000.0 / g_b.pages
                                           : 0.0,
          3);
    field("page_apply_ms_mean", g_b.pages ? static_cast<double>(clock_microseconds(clock, 0, g_apply_ticks)) /
                                                1000.0 / g_b.pages
                                          : 0.0,
          3);
    field("still_frame_ms_p50", still.p50, 3);
    field("still_cpu_ms_per_frame", still.cpu_ms, 3);
    field("still_main_allocs_per_frame", still.allocs, 1);
    field("jump_ms_p50", jumps.percentile(0.5), 3);
    field("jump_ms_p95", jumps.percentile(0.95), 3);
    field("jump_ms_max", jumps.maximum_ms(), 3);
    field("ram_above_data_mb", mb(after.working_set) - mb(base.working_set), 1);
    field("peak_above_data_mb", mb(end.peak_working_set) - mb(base.working_set), 1);
    field("private_mb", mb(end.private_bytes), 1);
    field("pages", g_b.pages, 0);
    field("pages_evicted", g_b.evicted, 0);
    field("jump_fetches", g_b.jumps_fetched, 0);
    field("resident_messages", f->view.store.count, 0);
    field("view_width_dips", g_b.scene_width_dips, 0);
    field("view_height_dips", g_b.scene_height_dips, 0);
    field("dpi", f->renderer.dpi, 0);
    field("scene_rows", f->scene.count, 0);
    field("text_quads", f->text.quad_count, 0);
    field("paint_quads", f->renderer.paint_stream.count, 0);
    field("alloc_hooks", hooks, 0);
    field("allocs_total", static_cast<double>(g_ui_allocs), 0);
    out.put("}\n");
    write_stdout(line, out.used);
    if (args.profile)
        write_profile();
    return out.overflow ? 30 : 0;
}

} // namespace
} // namespace maicord

#if defined(_WIN32)
extern "C" __declspec(noreturn) void WINAPI maicord_chat_list_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
#else
// The image links with --wrap=malloc,calloc,realloc (cmake/linux_benchmarks.cmake): its heap calls on the UI thread
// are counted here.
extern "C" {
void* __real_malloc(unsigned long bytes);
void* __real_calloc(unsigned long count, unsigned long bytes);
void* __real_realloc(void* block, unsigned long bytes);

[[gnu::used]] void* __wrap_malloc(unsigned long bytes) {
    if (maicord::current_thread_id() == maicord::g_ui_thread)
        maicord::g_ui_allocs = maicord::g_ui_allocs + 1;
    return __real_malloc(bytes);
}
[[gnu::used]] void* __wrap_calloc(unsigned long count, unsigned long bytes) {
    if (maicord::current_thread_id() == maicord::g_ui_thread)
        maicord::g_ui_allocs = maicord::g_ui_allocs + 1;
    return __real_calloc(count, bytes);
}
[[gnu::used]] void* __wrap_realloc(void* block, unsigned long bytes) {
    if (maicord::current_thread_id() == maicord::g_ui_thread)
        maicord::g_ui_allocs = maicord::g_ui_allocs + 1;
    return __real_realloc(block, bytes);
}
}

extern "C" [[noreturn, gnu::used]] void maicord_chat_list_benchmark_entry() noexcept {
    maicord::sys_exit_group(maicord::run());
}
#endif
