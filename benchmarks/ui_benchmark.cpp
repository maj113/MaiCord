#include "maicord/core/time.hpp"
#include "benchmark_output.hpp"
#include "maicord/app/message_composer_controller.hpp"
#include "maicord/app/message_media_controller.hpp"
#include "maicord/app/message_send_tracker.hpp"
#include "maicord/app/message_timeline.hpp"
#include "maicord/app/message_view_runtime.hpp"
#include "maicord/render/d3d11_device.hpp"
#include "maicord/render/d3d11_draw_pipeline.hpp"
#include "maicord/render/d3d11_gpu_timer.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/model/message_store.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/ui/navigation_view.hpp"
#include "maicord/ui/login_scene.hpp"
#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/login_presentation.hpp"
#include "maicord/ui/login_text_presentation.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/ui/message_rich_widgets.hpp"
#include "maicord/ui/message_scene.hpp"
#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/ui/member_list_scene.hpp"
#include "maicord/ui/message_text_coordinator.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/qr_code.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/shell_presentation.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/shell_text_presentation.hpp"
#include "maicord/ui/secure_edit.hpp"
#include "maicord/ui/secure_text_store.hpp"
#include "maicord/ui/text_flow_layout.hpp"
#include "maicord/ui/text_measure_cache.hpp"
#include "maicord/ui/virtual_list.hpp"

#include <textstor.h>

namespace maicord {
bool benchmark_text_form(Clock clock, void (*write)(const char*, u64, const char*) noexcept) noexcept;
bool benchmark_text_entry(Clock clock, void (*write)(const char*, u64, const char*) noexcept) noexcept;
bool benchmark_widget_scroll(Clock clock, void (*write)(const char*, u64, const char*) noexcept) noexcept;
bool benchmark_input_router(Clock clock, void (*write)(const char*, u64, const char*) noexcept) noexcept;
bool benchmark_navigation_shortcuts(Clock clock, void (*write)(const char*, u64, const char*) noexcept) noexcept;
bool benchmark_private_channels(Clock clock, void (*write)(const char*, u64, const char*) noexcept) noexcept;
namespace {

inline constexpr u32 kQuickWorkloadDivisor = 128;

// These are selected once at process entry. The default workload is deliberately
// short enough for routine verification; MAICORD_UI_BENCHMARK_EXHAUSTIVE=1 keeps
// the historical long-running counts for an explicitly requested baseline run.
u32 kLayoutIterations = 500'000;
u32 kLayoutReflowIterations = 1'000'000;
u32 kLoginLayoutIterations = 1'000'000;
u32 kPresentationIterations = 250'000;
u32 kLayoutValidationIterations = 200'000;
u32 kContentBoundsValidationIterations = 500'000;
u32 kSecureEditIterations = 500'000;
u32 kSecureTextStoreIterations = 500'000;
u32 kQrIterations = 20'000;
inline constexpr u32 kVariableItems = 65'536;
u32 kVariableIterations = 1'000'000;
inline constexpr u32 kProjectionChannels = 256;
inline constexpr u32 kGuildRailBenchmarkGuilds = 256;
u32 kProjectionIterations = 50'000;
inline constexpr u32 kMessageSceneItems = 100'000;
u32 kMessageSceneIterations = 10'000;
inline constexpr u32 kMemberListItems = 100'000;
u32 kMemberListIterations = 10'000;
u32 kRichMessageLayoutIterations = 100'000;
u32 kRichMessageMaterializeIterations = 100'000;
u32 kTextFlowIterations = 1'000'000;
u32 kWrapLayoutIterations = 250'000;
u32 kMessageViewRuntimeInitializeIterations = 50'000;
u32 kMessageViewRuntimeReindexIterations = 100'000;
u32 kMessageSendRoundTripIterations = 100'000;
u32 kMessageDraftSwitchIterations = 250'000;
u32 kMessageComposerSendRoundTripIterations = 100'000;
u32 kMessageComposerSelectionProjectionIterations = 1'000'000;
u32 kTextMeasureCacheHitIterations = 5'000'000;
u32 kTextMeasureCacheMissIterations = 1'000'000;
u32 kMessageTextMeasureIterations = 50'000;
u32 kMessageTextConvergenceIterations = 50'000;
u32 kMessageMediaSnapshotIterations = 100'000;
inline constexpr u32 kMessageMediaSnapshotRows = 192;
volatile u64 g_ui_checksum = 0;
u32 g_variable_heights[kVariableItems];
u64 g_variable_fenwick[kVariableItems];
GuildRecord g_projection_guilds[1];
ChannelRecord g_projection_channels[kProjectionChannels];
RoleRecord g_projection_roles[1];
PermissionOverwriteRecord g_projection_overwrites[16];
u16 g_projection_guild_order[1];
u16 g_projection_channel_order[kProjectionChannels];
u8 g_projection_names[32 * 1024];
GuildViewRow g_projection_guild_rows[1];
ChannelViewRow g_projection_channel_rows[kProjectionChannels];
u16 g_projection_scratch[kProjectionChannels];
u8 g_projection_visibility_scratch[kProjectionChannels];
ChannelSortKey g_projection_sort_keys[kProjectionChannels];
ShellNavigationRowWidget g_shell_navigation_rows[64];
UiWidget g_shell_navigation_widgets[258];
UiLayoutNode g_shell_navigation_layout[258];
GuildRailRowWidget g_guild_rail_rows[4];
UiWidget g_guild_rail_widgets[6];
UiLayoutNode g_guild_rail_layout[6];
GuildViewRow g_virtual_guild_source[kGuildRailBenchmarkGuilds];
GuildRailRowWidget g_virtual_guild_rows[32];
UiWidget g_virtual_guild_widgets[34];
UiLayoutNode g_virtual_guild_layout[34];
MessageRecord g_message_scene_records[kMessageSceneItems];
u32 g_message_scene_order[kMessageSceneItems];
u8 g_message_scene_text[1'000'000];
u32 g_message_scene_heights[kMessageSceneItems];
u64 g_message_scene_fenwick[kMessageSceneItems];
MessageSceneRowWidget g_message_scene_rows[32];
UiWidget g_message_scene_widgets[32 * kMessageSceneWidgetsPerRow + 1];
UiLayoutNode g_message_scene_layout[32 * kMessageSceneWidgetsPerRow + 1];
MessageRecord g_rich_message_records[1];
u32 g_rich_message_order[1];
u8 g_rich_message_text[512];
u32 g_rich_message_heights[1];
u64 g_rich_message_fenwick[1];
MessageRichRecord g_rich_message_records_detail[1];
u16 g_rich_message_order_detail[1];
u8 g_rich_message_text_detail[8 * 1024];
MessageSceneRowWidget g_rich_message_rows[1];
UiWidget g_rich_message_widgets[kMessageSceneWidgetsPerRow + 1];
UiLayoutNode g_rich_message_layout[kMessageSceneWidgetsPerRow + 1];
MessageTextCommand g_message_text_commands[kMaximumMessageTextCommands];
char g_message_text_utf8[kMaximumMessageTextUtf8Bytes];
TextStyleSpan g_message_text_styles[kMaximumMessageTextStyleSpans];
MessageMarkdownSemantic g_message_text_semantics[kMaximumMessageTextSemantics];
MessageTextMeasurement g_message_text_measurements[kMaximumMessageTextMeasurements];
MessageTextSpoilerReveal g_message_text_spoiler_reveals[kMaximumMessageTextSpoilerReveals];
u32 g_message_text_byte_styles[kMessageTextByteStyleScratchCapacity];
UiTextMeasureRecord g_text_measure_records[kMaximumMessageTextMeasureCacheRecords];
MessageViewRuntime g_message_view_runtime_benchmark{};
UserRecord g_member_records[kMemberListItems];
u32 g_member_order[kMemberListItems];
u8 g_member_names[2];
GuildMemberRecord g_member_guild_records[kMemberListItems];
u32 g_member_guild_order[kMemberListItems];
MemberListRow g_member_projection_rows[kMemberListItems];
MemberListRowWidget g_member_rows[32];
UiWidget g_member_widgets[1 + 32 * kMemberListWidgetsPerRow];
UiLayoutNode g_member_layout[1 + 32 * kMemberListWidgetsPerRow];
MessageRecord g_message_media_records[kMessageMediaSnapshotRows];
u32 g_message_media_order[kMessageMediaSnapshotRows];
u8 g_message_media_text[kMessageMediaSnapshotRows];
MessageRichRecord g_message_media_rich_records[1];
u16 g_message_media_rich_order[1];
u8 g_message_media_rich_text[2];
MessageSceneRowWidget g_message_media_scene_rows[kMessageMediaSnapshotRows];
MessageMediaVisibleRow g_message_media_visible_rows[kMessageMediaSnapshotRows];
MessageMediaAssetMark g_message_media_asset_marks[kMessageMediaAssetMarkCapacity];

u32 quick_iteration_count(u32 exhaustive_count) noexcept {
    const u32 scaled = (exhaustive_count + kQuickWorkloadDivisor - 1u) / kQuickWorkloadDivisor;
    return (scaled + 1u) & ~1u;
}

bool configure_benchmark_workload() noexcept {
    wchar_t value[2]{};
    const DWORD length = GetEnvironmentVariableW(L"MAICORD_UI_BENCHMARK_EXHAUSTIVE", value, 2);
    const bool exhaustive = length == 1 && value[0] == L'1';
    if (exhaustive) {
        return true;
    }

    kLayoutIterations = quick_iteration_count(kLayoutIterations);
    kLayoutReflowIterations = quick_iteration_count(kLayoutReflowIterations);
    kLoginLayoutIterations = quick_iteration_count(kLoginLayoutIterations);
    kPresentationIterations = quick_iteration_count(kPresentationIterations);
    kLayoutValidationIterations = quick_iteration_count(kLayoutValidationIterations);
    kContentBoundsValidationIterations = quick_iteration_count(kContentBoundsValidationIterations);
    kSecureEditIterations = quick_iteration_count(kSecureEditIterations);
    kSecureTextStoreIterations = quick_iteration_count(kSecureTextStoreIterations);
    kQrIterations = quick_iteration_count(kQrIterations);
    kVariableIterations = quick_iteration_count(kVariableIterations);
    kProjectionIterations = quick_iteration_count(kProjectionIterations);
    kMessageSceneIterations = quick_iteration_count(kMessageSceneIterations);
    kMemberListIterations = quick_iteration_count(kMemberListIterations);
    kRichMessageLayoutIterations = quick_iteration_count(kRichMessageLayoutIterations);
    kRichMessageMaterializeIterations = quick_iteration_count(kRichMessageMaterializeIterations);
    kTextFlowIterations = quick_iteration_count(kTextFlowIterations);
    kWrapLayoutIterations = quick_iteration_count(kWrapLayoutIterations);
    kMessageViewRuntimeInitializeIterations =
        quick_iteration_count(kMessageViewRuntimeInitializeIterations);
    kMessageViewRuntimeReindexIterations = quick_iteration_count(kMessageViewRuntimeReindexIterations);
    kMessageSendRoundTripIterations = quick_iteration_count(kMessageSendRoundTripIterations);
    kMessageDraftSwitchIterations = quick_iteration_count(kMessageDraftSwitchIterations);
    kMessageComposerSendRoundTripIterations =
        quick_iteration_count(kMessageComposerSendRoundTripIterations);
    kMessageComposerSelectionProjectionIterations =
        quick_iteration_count(kMessageComposerSelectionProjectionIterations);
    kTextMeasureCacheHitIterations = quick_iteration_count(kTextMeasureCacheHitIterations);
    kTextMeasureCacheMissIterations = quick_iteration_count(kTextMeasureCacheMissIterations);
    kMessageTextMeasureIterations = quick_iteration_count(kMessageTextMeasureIterations);
    kMessageTextConvergenceIterations = quick_iteration_count(kMessageTextConvergenceIterations);
    kMessageMediaSnapshotIterations = quick_iteration_count(kMessageMediaSnapshotIterations);
    return false;
}

bool benchmark_guid_equal(REFGUID left, REFGUID right) noexcept {
    if (left.Data1 != right.Data1 || left.Data2 != right.Data2 || left.Data3 != right.Data3) {
        return false;
    }
    for (u32 index = 0; index < 8; ++index) {
        if (left.Data4[index] != right.Data4[index]) {
            return false;
        }
    }
    return true;
}

struct BenchmarkTextStoreSink final : ITextStoreACPSink {
    LONG references = 1;
    ITextStoreACP* store = nullptr;
    u64 checksum = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** output) override {
        if (!output) {
            return E_POINTER;
        }
        *output = nullptr;
        if (!benchmark_guid_equal(riid, __uuidof(IUnknown)) &&
            !benchmark_guid_equal(riid, __uuidof(ITextStoreACPSink))) {
            return E_NOINTERFACE;
        }
        *output = static_cast<ITextStoreACPSink*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return static_cast<ULONG>(InterlockedDecrement(&references));
    }

    HRESULT STDMETHODCALLTYPE OnTextChange(DWORD, const TS_TEXTCHANGE*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange() override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnLayoutChange(TsLayoutCode, TsViewCookie) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStatusChange(DWORD) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnAttrsChange(LONG, LONG, ULONG, const TS_ATTRID*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnLockGranted(DWORD) override {
        LONG end = 0;
        ULONG fetched = 0;
        TS_SELECTION_ACP selection{};
        const HRESULT length_result = store->GetEndACP(&end);
        const HRESULT selection_result = store->GetSelection(TS_DEFAULT_SELECTION, 1, &selection, &fetched);
        if (FAILED(length_result) || FAILED(selection_result) || fetched != 1) {
            return E_FAIL;
        }
        checksum += static_cast<u64>(end) + static_cast<u64>(selection.acpEnd);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStartEditTransaction() override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnEndEditTransaction() override {
        return S_OK;
    }
};

struct BenchmarkTextMeasureState {
    u64 calls;
};

struct BenchmarkTextFinalizeState {
    u64 calls;
};

bool benchmark_text_measure_backend(void* opaque, const UiTextMeasureRequest& request,
                                    UiTextMeasureValue* value) noexcept {
    auto* state = static_cast<BenchmarkTextMeasureState*>(opaque);
    if (!state || !value || request.constraint.width <= 0.0f || request.constraint.height <= 0.0f) {
        return false;
    }
    ++state->calls;
    const u32 lines = 1u + request.text.bytes % 4u;
    *value = UiTextMeasureValue{UiSize{request.constraint.width, 18.0f * static_cast<float>(lines)}, 14.0f,
                                lines};
    return true;
}

bool benchmark_text_finalize(void* opaque, const Theme&) noexcept {
    auto* state = static_cast<BenchmarkTextFinalizeState*>(opaque);
    if (!state) {
        return false;
    }
    ++state->calls;
    return true;
}

bool benchmark_text_measure(void*, const UiLayoutMeasureRequest& request, UiSize* size,
                            float* baseline) noexcept {
    if (!size || !baseline || request.constraint.width <= 0.0f) {
        return false;
    }
    size->width = request.constraint.width;
    size->height = request.constraint.width < 100.0f ? 40.0f : 20.0f;
    *baseline = 15.0f;
    return true;
}

void write_runs(const char* name, u64 iterations, u64 elapsed_us) noexcept {
    write_metric(name, per_second(iterations, elapsed_us), "runs/s");
}

int run() noexcept {
    const bool exhaustive = configure_benchmark_workload();
    write_text(exhaustive ? "ui_benchmark_mode=exhaustive\r\n" : "ui_benchmark_mode=quick\r\n");
    UiScene scene{};
    if (!ui_scene_initialize_bootstrap(&scene)) {
        return 1;
    }
    const Clock clock = clock_initialize();
    u64 checksum = 0;
    MessageStore message_media_store{};
    MessageRichStore message_media_rich_store{};
    MessageScene message_media_scene{};
    MessageMediaController message_media_controller{};
    if (!message_store_initialize(&message_media_store,
                                  Slice<MessageRecord>{g_message_media_records, kMessageMediaSnapshotRows},
                                  Slice<u32>{g_message_media_order, kMessageMediaSnapshotRows},
                                  Slice<u8>{g_message_media_text, kMessageMediaSnapshotRows}) ||
        !message_rich_store_initialize(
            &message_media_rich_store, Slice<MessageRichRecord>{g_message_media_rich_records, 1},
            Slice<u16>{g_message_media_rich_order, 1}, Slice<u8>{g_message_media_rich_text, 2}) ||
        !message_media_controller_initialize(
            &message_media_controller,
            Slice<MessageMediaVisibleRow>{g_message_media_visible_rows, kMessageMediaSnapshotRows},
            Slice<MessageMediaAssetMark>{g_message_media_asset_marks, kMessageMediaAssetMarkCapacity})) {
        return 42;
    }
    for (u32 index = 0; index < kMessageMediaSnapshotRows; ++index) {
        const MessageStoreResult inserted = message_store_upsert(
            &message_media_store,
            MessageInput{1'000u + index, 7, 10u + (index & 15u), 1'000u + index, 0, utf8_literal("x"), 0});
        if (!inserted.ok()) {
            return 42;
        }
        g_message_media_scene_rows[index].source_index = index;
        g_message_media_scene_rows[index].message_id = 1'000u + index;
    }
    message_media_scene.rows =
        Slice<MessageSceneRowWidget>{g_message_media_scene_rows, kMessageMediaSnapshotRows};
    message_media_scene.first = 0;
    message_media_scene.count = kMessageMediaSnapshotRows;

    i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kMessageMediaSnapshotIterations; ++iteration) {
        for (u32 phase = 0; phase < 2; ++phase) {
            for (u32 index = 0; index < kMessageMediaSnapshotRows; ++index) {
                const MessageHandle handle = message_store_ordered_handle(message_media_store, index);
                const MessageRecord* message = message_store_resolve(message_media_store, handle);
                const MessageRichRecord* rich =
                    message ? message_rich_store_find(message_media_rich_store, message->id) : nullptr;
                const MessageSceneRowWidget* row = message_scene_find_row(message_media_scene, index);
                if (!message || !row) {
                    return 42;
                }
                checksum += message->id + row->message_id + static_cast<u64>(rich != nullptr) + phase;
            }
        }
    }
    const u64 message_media_legacy_resolve_us = clock_microseconds(clock, begin, clock_now());

    begin = clock_now();
    for (u32 iteration = 0; iteration < kMessageMediaSnapshotIterations; ++iteration) {
        message_media_controller_begin_pump(&message_media_controller);
        const MessageMediaControllerResult collected = message_media_controller_collect(
            &message_media_controller, message_media_store, message_media_rich_store, message_media_scene,
            UiVariableRange{0, kMessageMediaSnapshotRows, 0});
        if (!collected.ok() || collected.candidates != kMessageMediaSnapshotRows) {
            return 42;
        }
        for (u32 phase = 0; phase < 2; ++phase) {
            for (u32 index = 0; index < message_media_controller.row_count; ++index) {
                const MessageMediaVisibleRow& row = message_media_controller.rows[index];
                checksum +=
                    row.message->id + row.scene->message_id + static_cast<u64>(row.rich != nullptr) + phase;
            }
        }
    }
    const u64 message_media_snapshot_us = clock_microseconds(clock, begin, clock_now());
    begin = clock_now();
    for (u32 iteration = 0; iteration < kLayoutIterations; ++iteration) {
        const float width = (iteration & 1) != 0 ? 1280.0f : 600.0f;
        if (!ui_scene_layout_with_metrics(&scene, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics,
                                          width, 800.0f)) {
            return 2;
        }
        const UiNode* content = ui_scene_resolve(scene, ui_scene_find(scene, UiSemantic::content));
        if (!content) {
            return 3;
        }
        checksum += static_cast<u32>(content->rectangle.left) + scene.count;
    }
    const u64 layout_us = clock_microseconds(clock, begin, clock_now());
    const u32 scene_rebuilds = scene.layout_rebuilds;
    begin = clock_now();
    for (u32 iteration = 0; iteration < kLayoutReflowIterations; ++iteration) {
        const float width = (iteration & 1u) != 0 ? 1280.0f : 1200.0f;
        if (!ui_scene_layout_with_metrics(&scene, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics,
                                          width, 800.0f) ||
            scene.layout_rebuilds != scene_rebuilds) {
            return 22;
        }
        checksum += scene.layout_passes + scene.count;
    }
    const u64 layout_reflow_us = clock_microseconds(clock, begin, clock_now());

    UiScene friends_scene{};
    if (!ui_scene_initialize_bootstrap(&friends_scene) ||
        !ui_scene_layout_with_metrics(&friends_scene, UiShellView::friends, kFriendsShellLayoutMetrics,
                                      480.0f, 930.0f)) {
        return 23;
    }
    const u32 friends_rebuilds = friends_scene.layout_rebuilds;
    begin = clock_now();
    for (u32 iteration = 0; iteration < kLayoutReflowIterations; ++iteration) {
        const float width = (iteration & 1u) != 0 ? 1102.0f : 480.0f;
        if (!ui_scene_layout_with_metrics(&friends_scene, UiShellView::friends, kFriendsShellLayoutMetrics,
                                          width, 930.0f) ||
            friends_scene.layout_rebuilds != friends_rebuilds) {
            return 24;
        }
        checksum += friends_scene.layout_passes + friends_scene.count;
    }
    const u64 friends_reflow_us = clock_microseconds(clock, begin, clock_now());

    UiWidget measure_widgets[2]{};
    UiLayoutNode measure_nodes[2]{};
    UiWidgetManager measure_manager{};
    if (!ui_widget_manager_initialize(&measure_manager, Slice<UiWidget>{measure_widgets, 2},
                                      Slice<UiLayoutNode>{measure_nodes, 2}) ||
        !ui_widget_manager_set_measure_callback(&measure_manager, benchmark_text_measure, nullptr)) {
        return 25;
    }
    UiLayoutStyle measure_root_style = ui_layout_style();
    measure_root_style.flow = UiLayoutFlow::vertical;
    measure_root_style.width = ui_layout_fill();
    measure_root_style.height = ui_layout_fill();
    const UiWidgetResult measure_root = ui_widget_manager_add(
        &measure_manager, ui_widget_invalid_handle(),
        UiWidgetDesc{1, UiWidgetKind::container, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("MeasureRoot"), measure_root_style, UiSize{}, 0.0f, 1}});
    UiLayoutStyle measured_text_style = ui_layout_style();
    measured_text_style.width = ui_layout_fill();
    measured_text_style.height = ui_layout_content();
    measured_text_style.horizontal_alignment = UiLayoutAlignment::stretch;
    measured_text_style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_text_content);
    const UiWidgetResult measured_text =
        ui_widget_manager_add(&measure_manager, measure_root.handle,
                              UiWidgetDesc{2, UiWidgetKind::text, ui_widget_enabled,
                                           UiLayoutNodeSpec{utf8_literal("MeasuredText"),
                                                            measured_text_style, UiSize{}, 0.0f, 2}});
    if (!measure_root.ok() || !measured_text.ok() ||
        !ui_widget_manager_layout(&measure_manager, UiSize{80.0f, 100.0f}).ok()) {
        return 26;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kLayoutReflowIterations; ++iteration) {
        const float width = (iteration & 1u) != 0 ? 160.0f : 80.0f;
        if (!ui_widget_manager_layout(&measure_manager, UiSize{width, 100.0f}).ok()) {
            return 27;
        }
        checksum += measure_manager.layout.measure_passes;
    }
    const u64 constrained_reflow_us = clock_microseconds(clock, begin, clock_now());

    UiWidget wrap_widgets[18]{};
    UiLayoutNode wrap_nodes[18]{};
    UiWidgetManager wrap_manager{};
    if (!ui_widget_manager_initialize(&wrap_manager, Slice<UiWidget>{wrap_widgets, 18},
                                      Slice<UiLayoutNode>{wrap_nodes, 18})) {
        return 37;
    }
    UiLayoutStyle wrap_root_style = ui_layout_style();
    wrap_root_style.flow = UiLayoutFlow::vertical;
    wrap_root_style.width = ui_layout_fill();
    wrap_root_style.height = ui_layout_fill();
    const UiWidgetResult wrap_root = ui_widget_manager_add(
        &wrap_manager, ui_widget_invalid_handle(),
        UiWidgetDesc{1, UiWidgetKind::container, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("WrapRoot"), wrap_root_style, UiSize{}, 0.0f, 1}});
    UiLayoutStyle wrap_style = ui_layout_style();
    wrap_style.flow = UiLayoutFlow::horizontal_wrap;
    wrap_style.width = ui_layout_fill();
    wrap_style.height = ui_layout_content();
    wrap_style.gap = 6.0f;
    wrap_style.horizontal_alignment = UiLayoutAlignment::stretch;
    const UiWidgetResult wrap = ui_widget_manager_add(
        &wrap_manager, wrap_root.handle,
        UiWidgetDesc{2, UiWidgetKind::container, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("Wrap"), wrap_style, UiSize{}, 0.0f, 2}});
    UiLayoutStyle wrap_item_style = ui_layout_style();
    wrap_item_style.width = ui_layout_fixed(80.0f);
    wrap_item_style.height = ui_layout_fixed(24.0f);
    bool wrap_ready = wrap_root.ok() && wrap.ok();
    for (u32 index = 0; index < 16; ++index) {
        wrap_ready =
            wrap_ready &&
            ui_widget_manager_add(
                &wrap_manager, wrap.handle,
                UiWidgetDesc{
                    3 + index, UiWidgetKind::button,
                    static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable),
                    UiLayoutNodeSpec{utf8_literal("WrapItem"), wrap_item_style, UiSize{}, 0.0f, 3 + index}})
                .ok();
    }
    if (!wrap_ready || !ui_widget_manager_layout(&wrap_manager, UiSize{720.0f, 512.0f}).ok()) {
        return 37;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kWrapLayoutIterations; ++iteration) {
        const float width = (iteration & 1u) != 0 ? 720.0f : 280.0f;
        if (!ui_widget_manager_layout(&wrap_manager, UiSize{width, 512.0f}).ok()) {
            return 37;
        }
        checksum += wrap_manager.layout.measure_passes + static_cast<u32>(wrap_nodes[17].rectangle.top);
    }
    const u64 wrap_layout_us = clock_microseconds(clock, begin, clock_now());

    const UiNode* friends_title =
        ui_scene_resolve(friends_scene, ui_scene_find(friends_scene, UiSemantic::title_bar));
    const UiNode* friends_navigation =
        ui_scene_resolve(friends_scene, ui_scene_find(friends_scene, UiSemantic::channel_navigation));
    const UiNode* friends_messages =
        ui_scene_resolve(friends_scene, ui_scene_find(friends_scene, UiSemantic::message_view));
    if (!friends_title || !friends_navigation || !friends_messages) {
        return 28;
    }
    const UiRect friends_title_content = ui_scene_content_rectangle(friends_scene, UiSemantic::title_bar);
    const UiRect friends_navigation_content =
        ui_scene_content_rectangle(friends_scene, UiSemantic::channel_navigation);
    const UiRect friends_messages_content =
        ui_scene_content_rectangle(friends_scene, UiSemantic::message_view);
    const UiLayoutContentBounds content_bounds[3]{
        UiLayoutContentBounds{friends_scene.storage.widgets[friends_title->widget.slot].layout,
                              UiRect{friends_title_content.left + 8.0f, friends_title_content.top + 2.0f,
                                     friends_title_content.left + 80.0f, friends_title_content.top + 18.0f},
                              UiLayoutContentKind::text, ui_layout_content_bounds_collision},
        UiLayoutContentBounds{
            friends_scene.storage.widgets[friends_navigation->widget.slot].layout,
            UiRect{friends_navigation_content.left + 8.0f, friends_navigation_content.top + 8.0f,
                   friends_navigation_content.left + 120.0f, friends_navigation_content.top + 28.0f},
            UiLayoutContentKind::text, ui_layout_content_bounds_collision},
        UiLayoutContentBounds{
            friends_scene.storage.widgets[friends_messages->widget.slot].layout,
            UiRect{friends_messages_content.left + 8.0f, friends_messages_content.top + 8.0f,
                   friends_messages_content.left + 120.0f, friends_messages_content.top + 28.0f},
            UiLayoutContentKind::text, ui_layout_content_bounds_collision},
    };
    UiLayoutDiagnostic content_diagnostics[8]{};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kContentBoundsValidationIterations; ++iteration) {
        const UiLayoutValidationResult validated = ui_layout_engine_validate_content_bounds(
            friends_scene.widgets.layout, ConstSlice<UiLayoutContentBounds>{content_bounds, 3},
            Slice<UiLayoutDiagnostic>{content_diagnostics, 8});
        if (!validated.clean()) {
            return 29;
        }
        checksum += validated.count + 1;
    }
    const u64 content_bounds_validation_us = clock_microseconds(clock, begin, clock_now());

    LoginScene login_scene{};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kLoginLayoutIterations; ++iteration) {
        const float width = (iteration & 1u) != 0 ? 1280.0f : 640.0f;
        if (!login_scene_layout(&login_scene, width, 800.0f)) {
            return 12;
        }
        checksum += static_cast<u64>(login_scene_rectangle(login_scene, LoginElement::card).left) +
                    (login_scene.qr_visible ? 1u : 0u);
    }
    const u64 login_layout_us = clock_microseconds(clock, begin, clock_now());

    login_scene.mfa_capabilities =
        static_cast<u8>(login_mfa_capability_totp | login_mfa_capability_sms | login_mfa_capability_backup |
                        login_mfa_capability_webauthn);
    login_scene.selected_mfa_capability = login_mfa_capability_sms;
    begin = clock_now();
    for (u32 iteration = 0; iteration < kLoginLayoutIterations; ++iteration) {
        const float width = (iteration & 1u) != 0 ? 1280.0f : 640.0f;
        login_scene.selected_mfa_capability = static_cast<u8>(1u << (iteration & 3u));
        if (!login_scene_layout(&login_scene, width, 800.0f)) {
            return 15;
        }
        checksum += static_cast<u64>(login_scene_rectangle(login_scene, LoginElement::mfa_sms).left) +
                    login_scene.selected_mfa_capability;
    }
    const u64 login_mfa_layout_us = clock_microseconds(clock, begin, clock_now());

    login_scene.mfa_capabilities = login_mfa_capability_none;
    login_scene.selected_mfa_capability = login_mfa_capability_none;
    if (!login_scene_layout(&login_scene, 1280.0f, 800.0f)) {
        return 20;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kLoginLayoutIterations; ++iteration) {
        login_scene.focus = (iteration & 1u) != 0 ? LoginControl::login : LoginControl::password;
        if (!login_scene_layout(&login_scene, 1280.0f, 800.0f)) {
            return 20;
        }
        checksum += login_scene.layout_rebuilds + static_cast<u32>(login_scene.focus);
    }
    const u64 login_cached_layout_us = clock_microseconds(clock, begin, clock_now());
    const u32 login_rebuilds = login_scene.layout_rebuilds;
    begin = clock_now();
    for (u32 iteration = 0; iteration < kLoginLayoutIterations; ++iteration) {
        const float width = (iteration & 1u) != 0 ? 1280.0f : 1200.0f;
        if (!login_scene_layout(&login_scene, width, 800.0f) ||
            login_scene.layout_rebuilds != login_rebuilds) {
            return 23;
        }
        checksum += login_scene.layout_passes + login_scene.widgets.count;
    }
    const u64 login_reflow_us = clock_microseconds(clock, begin, clock_now());

    PaintStream login_paint{};
    const LoginPresentationInput login_presentation{
        ConstSlice<UiRect>{}, ConstSlice<LoginCompositionGeometry>{}, UiRect{}, nullptr, false, false};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kPresentationIterations; ++iteration) {
        login_scene.action_disabled = (iteration & 1u) != 0;
        login_scene.submit_disabled = login_scene.action_disabled;
        if (!login_presentation_build(login_scene, login_presentation, kDefaultTheme, 1280.0f, 800.0f, 96,
                                      &login_paint)) {
            return 24;
        }
        checksum += login_paint.count + login_paint.batch_count;
    }
    const u64 login_presentation_us = clock_microseconds(clock, begin, clock_now());

    UiTextCommand login_text_commands[kMaximumLoginTextCommands]{};
    UiTextCommandStream login_text_stream{};
    LoginView login_text_view{utf8_literal("account@example.com"),
                                    utf8_literal("********"),
                                    {},
                                    0,
                                    0,
                                    false,
                                    LoginControl::login,
                                    LoginStatus::submitting,
                                    login_mfa_capability_none,
                                    login_mfa_capability_none,
                                    0,
                                    false,
                                    nullptr,
                                    false,
                                    false,
                                    false,
                                    true};
    login_scene.focus = login_text_view.focus;
    login_scene.recovery_disabled = login_view_recovery_disabled(login_text_view);
    login_scene.action_disabled = login_view_action_disabled(login_text_view);
    login_scene.submit_disabled = login_view_submit_disabled(login_text_view);
    if (!login_scene_layout(&login_scene, 1280.0f, 800.0f)) {
        return 30;
    }
    login_text_view.focus = login_scene.focus;
    if (!ui_text_command_stream_initialize(
            &login_text_stream, Slice<UiTextCommand>{login_text_commands, kMaximumLoginTextCommands})) {
        return 30;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kPresentationIterations; ++iteration) {
        if (!login_text_presentation_build(login_scene, login_text_view, kDefaultTheme,
                                           &login_text_stream)) {
            return 30;
        }
        checksum += login_text_stream.count +
                    login_text_stream.storage[iteration % login_text_stream.count].text.bytes;
    }
    const u64 login_text_presentation_us = clock_microseconds(clock, begin, clock_now());

    UiLayoutDiagnostic layout_diagnostics[32]{};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kLayoutValidationIterations; ++iteration) {
        const UiLayoutValidationResult validation =
            ui_layout_engine_validate(login_scene.widgets.layout, Slice<UiLayoutDiagnostic>{layout_diagnostics, 32});
        if (!validation.clean()) {
            return 21;
        }
        checksum += validation.count + login_scene.widgets.count;
    }
    const u64 layout_validation_us = clock_microseconds(clock, begin, clock_now());

    char qr_payload[] = "https://discord.com/ra/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    QrCode qr{};
    QrCodeRect qr_rectangles[384]{};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kQrIterations; ++iteration) {
        qr_payload[sizeof(qr_payload) - 2] = (iteration & 1u) != 0 ? 'A' : 'B';
        if (!qr_code_encode_v5_m(ConstSlice<u8>{reinterpret_cast<const u8*>(qr_payload),
                                                static_cast<u32>(sizeof(qr_payload) - 1)},
                                 &qr)) {
            return 16;
        }
        const QrCodeRectResult compacted = qr_code_compact_rects(
            qr, Slice<QrCodeRect>{qr_rectangles,
                                  static_cast<u32>(sizeof(qr_rectangles) / sizeof(qr_rectangles[0]))});
        if (!compacted.ok()) {
            return 17;
        }
        checksum += qr.mask + compacted.count + qr.modules[iteration % kQrCodeStorageBytes];
    }
    const u64 qr_us = clock_microseconds(clock, begin, clock_now());

    SecureEdit secure_edit{};
    if (!secure_edit_initialize(&secure_edit, 512, false)) {
        return 13;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kSecureEditIterations; ++iteration) {
        if (secure_edit_assign(&secure_edit, utf8_literal("native.user")) ==
                SecureEditResult::invalid_input ||
            secure_edit_move(&secure_edit, SecureEditMove::previous, false) ==
                SecureEditResult::invalid_input ||
            secure_edit_insert_scalar(&secure_edit, 'x') != SecureEditResult::changed ||
            secure_edit_select_all(&secure_edit) == SecureEditResult::invalid_input ||
            secure_edit_delete_forward(&secure_edit) != SecureEditResult::changed) {
            secure_edit_release(&secure_edit);
            return 14;
        }
        checksum += secure_edit.revision;
    }
    const u64 secure_edit_us = clock_microseconds(clock, begin, clock_now());

    if (secure_edit_assign(
            &secure_edit, utf8_literal("native.user@example.test/"
                                       "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ")) !=
            SecureEditResult::changed ||
        secure_edit_select_all(&secure_edit) != SecureEditResult::changed) {
        secure_edit_release(&secure_edit);
        return 18;
    }
    char edit_window_text[192]{};
    SecureEditRenderWindow edit_window{};
    u32 edit_window_bytes = 0;
    begin = clock_now();
    for (u32 iteration = 0; iteration < kSecureEditIterations; ++iteration) {
        if (secure_edit_render_window(
                secure_edit, 47u + (iteration & 1u),
                Slice<char>{edit_window_text, static_cast<u32>(sizeof(edit_window_text))},
                &edit_window_bytes, &edit_window) != SecureEditResult::unchanged) {
            secure_edit_release(&secure_edit);
            return 18;
        }
        checksum += edit_window_bytes + edit_window.first_codepoint + edit_window.caret;
    }
    const u64 secure_edit_render_us = clock_microseconds(clock, begin, clock_now());

    constexpr u16 composition_text[] = {0xD83Du, 0xDE00u};
    u16 composition_readback[8]{};
    SecureEditUtf16Change composition_change{};
    u32 composition_units = 0;
    u32 composition_written = 0;
    u32 composition_next = 0;
    begin = clock_now();
    for (u32 iteration = 0; iteration < kSecureEditIterations; ++iteration) {
        if (secure_edit_assign(&secure_edit, utf8_literal("A\xC3\xA9")) ==
                SecureEditResult::invalid_input ||
            secure_edit_utf16_length(secure_edit, &composition_units) != SecureEditResult::unchanged ||
            secure_edit_set_utf16_selection(&secure_edit, SecureEditUtf16Selection{1, 2}) ==
                SecureEditResult::invalid_utf16 ||
            secure_edit_replace_utf16(
                &secure_edit, 1, 2,
                ConstSlice<u16>{composition_text,
                                static_cast<u32>(sizeof(composition_text) / sizeof(composition_text[0]))},
                &composition_change) != SecureEditResult::changed ||
            secure_edit_copy_utf16(
                secure_edit, 0, 3,
                Slice<u16>{composition_readback, static_cast<u32>(sizeof(composition_readback) /
                                                                  sizeof(composition_readback[0]))},
                &composition_written, &composition_next) != SecureEditResult::unchanged) {
            secure_edit_release(&secure_edit);
            return 19;
        }
        checksum += composition_units + composition_change.new_end + composition_written +
                    composition_next + composition_readback[iteration & 3u];
    }
    const u64 secure_edit_acp_us = clock_microseconds(clock, begin, clock_now());

    SecureTextStore text_store_owner{};
    BenchmarkTextStoreSink text_store_sink{};
    if (!secure_text_store_initialize(
            &text_store_owner,
            SecureTextStoreConfig{reinterpret_cast<HWND>(1), nullptr, nullptr, nullptr}) ||
        !secure_text_store_bind(&text_store_owner, &secure_edit, SecureTextInputScope::text)) {
        secure_text_store_release(&text_store_owner);
        secure_edit_release(&secure_edit);
        return 42;
    }
    auto* text_store = static_cast<ITextStoreACP*>(secure_text_store_test_interface(&text_store_owner));
    text_store_sink.store = text_store;
    if (!text_store ||
        FAILED(text_store->AdviseSink(__uuidof(ITextStoreACPSink), &text_store_sink, TS_AS_ALL_SINKS))) {
        secure_text_store_release(&text_store_owner);
        secure_edit_release(&secure_edit);
        return 42;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kSecureTextStoreIterations; ++iteration) {
        HRESULT session = E_FAIL;
        if (FAILED(text_store->RequestLock(TS_LF_SYNC | TS_LF_READ, &session)) || FAILED(session)) {
            text_store->UnadviseSink(&text_store_sink);
            secure_text_store_release(&text_store_owner);
            secure_edit_release(&secure_edit);
            return 42;
        }
    }
    const u64 secure_text_store_us = clock_microseconds(clock, begin, clock_now());
    checksum += text_store_sink.checksum;
    text_store->UnadviseSink(&text_store_sink);
    secure_text_store_release(&text_store_owner);
    secure_edit_release(&secure_edit);

    for (u32 index = 0; index < kMemberListItems; ++index) {
        g_member_records[index].id = static_cast<u64>(index) + 1;
        g_member_records[index].occupied = true;
        g_member_order[index] = index;
        g_member_guild_records[index].guild_id = 1;
        g_member_guild_records[index].user_id = static_cast<u64>(index) + 1;
        g_member_guild_records[index].status = GuildMemberStatus::online;
        g_member_guild_records[index].occupied = true;
        g_member_guild_order[index] = index;
        g_member_projection_rows[index] = MemberListRow{
            static_cast<u64>(index) + 1,      kMemberListOnlineGroup,   index, 0, 0,
            GuildMemberStatus::online, MemberListRowKind::member};
    }
    UserStore member_users{};
    member_users.records = g_member_records;
    member_users.order = g_member_order;
    member_users.names = g_member_names;
    member_users.capacity = kMemberListItems;
    member_users.count = kMemberListItems;
    member_users.name_bank_bytes = 1;
    member_users.revision = 1;
    GuildMemberStore member_guild_store{};
    member_guild_store.records = g_member_guild_records;
    member_guild_store.order = g_member_guild_order;
    member_guild_store.capacity = kMemberListItems;
    member_guild_store.count = kMemberListItems;
    member_guild_store.revision = 1;
    MemberListView member_projection{};
    member_projection.rows = Slice<MemberListRow>{g_member_projection_rows, kMemberListItems};
    member_projection.guild_id = 1;
    member_projection.count = kMemberListItems;
    member_projection.member_count = kMemberListItems;
    member_projection.revision = 1;
    GuildRecord member_guild{};
    ChatStateStore member_state{};
    member_state.guilds = &member_guild;
    MemberListScene member_scene{};
    const UiRect member_viewport{1'000.0f, 48.0f, 1'240.0f, 768.0f};
    if (!member_list_scene_initialize(
            &member_scene, Slice<MemberListRowWidget>{g_member_rows, 32},
            Slice<UiWidget>{g_member_widgets, 1 + 32 * kMemberListWidgetsPerRow},
            Slice<UiLayoutNode>{g_member_layout, 1 + 32 * kMemberListWidgetsPerRow})) {
        return 43;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kMemberListIterations; ++iteration) {
        const float scroll = static_cast<float>((static_cast<u64>(iteration) * 6'209ull) %
                                                (static_cast<u64>(kMemberListItems) * 44ull));
        const MemberListSceneResult materialized =
            member_list_scene_update(&member_scene, &member_projection, &member_guild_store, &member_users,
                                     &member_state, member_viewport, scroll);
        if (!materialized.ok() || materialized.count > 32 ||
            member_scene.widgets.count > 1 + 32 * kMemberListWidgetsPerRow) {
            return 43;
        }
        checksum += materialized.first + materialized.count + member_scene.widgets.count;
    }
    const u64 member_list_us = clock_microseconds(clock, begin, clock_now());

    UiVariableList variable{};
    if (!ui_variable_list_initialize(&variable, Slice<u32>{g_variable_heights, kVariableItems},
                                     Slice<u64>{g_variable_fenwick, kVariableItems}, kVariableItems,
                                     ui_extent_units(68.0f))) {
        return 4;
    }
    for (u32 index = 0; index < kVariableItems; index += 7) {
        if (!ui_variable_list_set_height(&variable, index, ui_extent_units(92.0f))) {
            return 5;
        }
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kVariableIterations; ++iteration) {
        const u64 scroll = (static_cast<u64>(iteration) * 6'209ull) % variable.total_units;
        const UiVariableRange range = ui_variable_list_range(variable, scroll, ui_extent_units(720.0f), 2);
        checksum += range.first + range.count +
                    static_cast<u64>(range.first_offset_units < 0 ? -range.first_offset_units
                                                                  : range.first_offset_units);
    }
    const u64 variable_us = clock_microseconds(clock, begin, clock_now());

    ChatStateStore state{};
    NavigationView navigation{};
    if (!chat_state_store_initialize(
            &state, Slice<GuildRecord>{g_projection_guilds, 1},
            Slice<u16>{g_projection_guild_order, 1},
            Slice<ChannelRecord>{g_projection_channels, kProjectionChannels},
            Slice<u16>{g_projection_channel_order, kProjectionChannels},
            Slice<u8>{g_projection_names, static_cast<u32>(sizeof(g_projection_names))}) ||
        !chat_state_store_bind_permissions(
            &state, Slice<RoleRecord>{g_projection_roles, 1},
            Slice<PermissionOverwriteRecord>{g_projection_overwrites, 16}) ||
        !chat_state_store_upsert_guild(
             &state, GuildInput{1, utf8_literal("benchmark"), GuildMutationKind::create, 999})
             .ok()) {
        return 6;
    }
    for (u32 index = 0; index < kProjectionChannels; ++index) {
        const bool category = index < 16;
        const u64 parent_id = category ? 0 : 1'000 + (index & 15u);
        if (!chat_state_store_upsert_channel(
                 &state, ChannelInput{1'000 + index, 1, parent_id, utf8_literal("channel"),
                                             (index * 97u) % kProjectionChannels, 0,
                                             static_cast<u8>(category ? 4 : 0)})
                 .ok()) {
            return 7;
        }
    }
    if (!chat_state_store_upsert_role(
            &state, RoleInput{1, 1,
                                     kPermissionViewChannel | kPermissionReadMessageHistory |
                                         kPermissionSendMessages | kPermissionAttachFiles}) ||
        !chat_state_store_set_self_roles(&state, 1, 999, 1, ConstSlice<u64>{nullptr, 0})) {
        return 8;
    }
    for (u32 index = 0; index < 16; ++index) {
        if (!chat_state_store_append_channel_overwrite(
                &state, 1'000 + index,
                PermissionOverwriteInput{1, 0, kPermissionViewChannel, 0})) {
            return 8;
        }
    }
    chat_state_store_recompute_guild_permissions(&state, 1);
    for (u32 index = 16; index < kProjectionChannels; ++index) {
        if (!chat_state_store_note_message(&state, 1'000 + index, 2'000'000 + index, 2,
                                              (index % 7u) == 0, false)) {
            return 9;
        }
    }
    if (!navigation_view_initialize(
            &navigation, Slice<GuildViewRow>{g_projection_guild_rows, 1},
            Slice<ChannelViewRow>{g_projection_channel_rows, kProjectionChannels},
            Slice<u16>{g_projection_scratch, kProjectionChannels},
            Slice<u8>{g_projection_visibility_scratch, kProjectionChannels},
            Slice<ChannelSortKey>{g_projection_sort_keys, kProjectionChannels})) {
        return 9;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kProjectionIterations; ++iteration) {
        const NavigationViewResult projected = navigation_view_rebuild(&navigation, state);
        if (!projected.ok() || projected.channel_count != kProjectionChannels) {
            return 10;
        }
        checksum += navigation.channel_rows[iteration & (kProjectionChannels - 1)].id;
    }
    const u64 projection_us = clock_microseconds(clock, begin, clock_now());
    for (u32 index = 0; index < kGuildRailBenchmarkGuilds; ++index) {
        g_virtual_guild_source[index] = GuildViewRow{
            100'000 + index, 1, (index % 11u) == 0 ? 1u : 0u, 1, index == 0, false, (index % 7u) == 0};
    }
    NavigationView virtual_guild_navigation{};
    virtual_guild_navigation.guild_rows = g_virtual_guild_source;
    virtual_guild_navigation.guild_capacity = kGuildRailBenchmarkGuilds;
    virtual_guild_navigation.guild_count = kGuildRailBenchmarkGuilds;
    virtual_guild_navigation.selected_guild_id = 100'000;
    virtual_guild_navigation.revision = 1;
    virtual_guild_navigation.guild_revision = 1;
    GuildRailScene virtual_guild_scene{};
    const UiRect virtual_guild_viewport{0.0f, 0.0f, 72.0f, 800.0f};
    if (!guild_rail_scene_initialize(&virtual_guild_scene,
                                     Slice<GuildRailRowWidget>{g_virtual_guild_rows, 32},
                                     Slice<UiWidget>{g_virtual_guild_widgets, 34},
                                     Slice<UiLayoutNode>{g_virtual_guild_layout, 34})) {
        return 25;
    }
    if (!guild_rail_scene_update(&virtual_guild_scene, &virtual_guild_navigation, virtual_guild_viewport, 0.0f).ok()) {
        return 25;
    }
    const float virtual_guild_max_scroll =
        virtual_guild_scene.content_height - ui_rect_height(virtual_guild_viewport);
    begin = clock_now();
    for (u32 iteration = 0; iteration < kPresentationIterations; ++iteration) {
        const float scroll = static_cast<float>((static_cast<u64>(iteration) * 17ull) %
                                                static_cast<u64>(virtual_guild_max_scroll));
        const GuildRailSceneResult guild_layout = guild_rail_scene_update(
            &virtual_guild_scene, &virtual_guild_navigation, virtual_guild_viewport, scroll);
        if (!guild_layout.ok()) {
            return 25;
        }
        checksum += guild_layout.first + guild_layout.count + static_cast<u32>(guild_layout.scroll_dips);
    }
    const u64 guild_rail_layout_us = clock_microseconds(clock, begin, clock_now());
    GuildRailScene guild_rail_scene{};
    const UiNode* guild_rail_viewport =
        ui_scene_resolve(scene, ui_scene_find(scene, UiSemantic::guild_navigation));
    ShellNavigationScene shell_navigation_scene{};
    const UiNode* shell_navigation_viewport =
        ui_scene_resolve(scene, ui_scene_find(scene, UiSemantic::channel_navigation));
    if (!guild_rail_viewport || !shell_navigation_viewport ||
        !guild_rail_scene_initialize(&guild_rail_scene, Slice<GuildRailRowWidget>{g_guild_rail_rows, 4},
                                     Slice<UiWidget>{g_guild_rail_widgets, 6},
                                     Slice<UiLayoutNode>{g_guild_rail_layout, 6}) ||
        !guild_rail_scene_update(&guild_rail_scene, &navigation, guild_rail_viewport->rectangle, 0.0f)
             .ok() ||
        !shell_navigation_scene_initialize(&shell_navigation_scene,
                                           Slice<ShellNavigationRowWidget>{g_shell_navigation_rows, 64},
                                           Slice<UiWidget>{g_shell_navigation_widgets, 258},
                                           Slice<UiLayoutNode>{g_shell_navigation_layout, 258})) {
        return 25;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kPresentationIterations; ++iteration) {
        const ShellNavigationSceneResult navigation_layout = shell_navigation_scene_update(
            &shell_navigation_scene, &navigation, shell_navigation_viewport->rectangle,
            static_cast<float>(iteration & 1023u));
        if (!navigation_layout.ok()) {
            return 25;
        }
        checksum += navigation_layout.first + navigation_layout.count +
                    static_cast<u32>(navigation_layout.scroll_dips);
    }
    const u64 shell_navigation_layout_us = clock_microseconds(clock, begin, clock_now());
    if (!shell_navigation_scene_update(&shell_navigation_scene, &navigation,
                                       shell_navigation_viewport->rectangle, 128.0f)
             .ok()) {
        return 25;
    }
    PaintStream shell_paint{};
    ShellPresentationRegions shell_regions{};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kPresentationIterations; ++iteration) {
        if (!shell_presentation_build_chrome(scene, &navigation, &guild_rail_scene, &shell_navigation_scene,
                                             kDefaultTheme, 1280.0f, 800.0f, &shell_paint,
                                             &shell_regions)) {
            return 25;
        }
        checksum += shell_paint.count + shell_paint.batch_count +
                    static_cast<u32>(shell_navigation_scene.scroll_dips);
    }
    const u64 shell_presentation_us = clock_microseconds(clock, begin, clock_now());
    UiTextCommand shell_text_commands[kMaximumShellChromeTextCommands]{};
    char shell_text_owned_bytes[256];
    UiTextCommandStream shell_text_stream{};
    if (!ui_text_command_stream_initialize(
            &shell_text_stream,
            Slice<UiTextCommand>{shell_text_commands, kMaximumShellChromeTextCommands},
            Slice<char>{shell_text_owned_bytes, sizeof(shell_text_owned_bytes)})) {
        return 31;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kPresentationIterations; ++iteration) {
        if (!shell_text_presentation_build(scene, state, navigation, kDefaultTheme, shell_navigation_scene,
                                           &shell_text_stream)) {
            return 31;
        }
        checksum += shell_text_stream.count +
                    (shell_text_stream.count != 0
                         ? shell_text_stream.storage[iteration % shell_text_stream.count].text.bytes
                         : 0);
    }
    const u64 shell_text_presentation_us = clock_microseconds(clock, begin, clock_now());

    MessageStore message_store{};
    if (!message_store_initialize(
            &message_store, Slice<MessageRecord>{g_message_scene_records, kMessageSceneItems},
            Slice<u32>{g_message_scene_order, kMessageSceneItems},
            Slice<u8>{g_message_scene_text, static_cast<u32>(sizeof(g_message_scene_text))})) {
        return 32;
    }
    for (u32 index = 0; index < kMessageSceneItems; ++index) {
        if (!message_store_upsert(&message_store,
                                  MessageInput{500'000 + index, 7, 9, 1'700'000'000'000ull + index, 0,
                                               utf8_literal("benchmark"), 0})
                 .ok()) {
            return 32;
        }
    }
    UiVariableList message_list{};
    MessageTimeline message_timeline{};
    const u32 message_default_units = ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height);
    const UiRect message_viewport{0.0f, 0.0f, 728.0f, 720.0f};

    begin = clock_now();
    for (u32 iteration = 0; iteration < kMessageViewRuntimeInitializeIterations; ++iteration) {
        if (!message_view_runtime_initialize(&g_message_view_runtime_benchmark, message_default_units,
                                             ui_extent_units(720.0f), (iteration & 1u) != 0)
                 .ok()) {
            return 34;
        }
        checksum += g_message_view_runtime_benchmark.store.capacity +
                    g_message_view_runtime_benchmark.list.capacity;
    }
    const u64 message_view_runtime_initialize_us = clock_microseconds(clock, begin, clock_now());

    for (u32 index = 0; index < kMessageViewItemCapacity; ++index) {
        if (!message_store_upsert(&g_message_view_runtime_benchmark.store,
                                  MessageInput{600'000 + index, 7, 9, 1'700'000'100'000ull + index, 0,
                                               utf8_literal("runtime"), 0})
                 .ok()) {
            return 34;
        }
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kMessageViewRuntimeReindexIterations; ++iteration) {
        const u32 item_units = message_default_units + (iteration & 1u);
        if (!message_view_runtime_reindex(&g_message_view_runtime_benchmark, item_units,
                                          ui_extent_units(720.0f), (iteration & 1u) != 0)
                 .ok()) {
            return 34;
        }
        checksum += g_message_view_runtime_benchmark.list.total_units +
                    g_message_view_runtime_benchmark.store.count;
    }
    const u64 message_view_runtime_reindex_us = clock_microseconds(clock, begin, clock_now());

    MessageRecord send_records[2]{};
    u32 send_order[2]{};
    u8 send_text[4 * 1024]{};
    MessageStore send_store{};
    u32 send_heights[2]{};
    u64 send_fenwick[2]{};
    UiVariableList send_list{};
    MessageTimeline send_timeline{};
    MessageSendTracker send_tracker{};
    if (!message_store_initialize(&send_store, Slice<MessageRecord>{send_records, 2},
                                  Slice<u32>{send_order, 2}, Slice<u8>{send_text, sizeof(send_text)}) ||
        !ui_variable_list_initialize(&send_list, Slice<u32>{send_heights, 2}, Slice<u64>{send_fenwick, 2},
                                     0, message_default_units) ||
        !message_timeline_initialize(&send_timeline, &send_store, &send_list, message_default_units,
                                     ui_extent_units(720.0f), true) ||
        !message_send_tracker_initialize(&send_tracker)) {
        return 42;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kMessageSendRoundTripIterations; ++iteration) {
        const u64 started = static_cast<u64>(iteration) * 3u + 1u;
        const MessageSendBeginResult sent =
            message_send_tracker_begin(&send_tracker, &send_timeline, 7, 9, utf8_literal("send"),
                                       1'725'012'296'900ull + iteration, started);
        const MessageSendObserveResult http =
            message_send_tracker_observe_http(&send_tracker, &send_timeline, sent.nonce, 200,
                                              MessageSendHttpDisposition::success, started + 1);
        const MessageSendObserveResult gateway = message_send_tracker_observe_gateway(
            &send_tracker, &send_timeline, sent.nonce, 7, 9, 900'000 + iteration, started + 2);
        MessageSendCompletion completed{};
        if (!sent.ok() || !http.ok() || !gateway.ok() || !gateway.completed ||
            !message_send_tracker_take_completion(&send_tracker, &completed) ||
            completed.kind != MessageSendCompletionKind::accepted || send_store.count != 0 ||
            send_list.count != 0) {
            return 42;
        }
        checksum += completed.nonce + completed.authoritative_id + completed.api_elapsed_ms +
                    completed.gateway_elapsed_ms;
    }
    const u64 message_send_round_trip_us = clock_microseconds(clock, begin, clock_now());

    char composer_text[128]{};
    MessageComposer composer{};
    MessageDraftRecord draft_records[4]{};
    u8 draft_text[128]{};
    MessageDraftStore drafts{};
    u8 request_text[kMessageComposerMaximumBytes]{};
    MessageComposerController composer_controller{};
    if (!message_composer_initialize(&composer, Slice<char>{composer_text, 128}) ||
        !message_draft_store_initialize(&drafts, Slice<MessageDraftRecord>{draft_records, 4},
                                        Slice<u8>{draft_text, 128}) ||
        !message_composer_controller_initialize(&composer_controller, &composer, &drafts, &send_timeline,
                                                Slice<u8>{request_text, kMessageComposerMaximumBytes})) {
        return 43;
    }
    message_composer_reset(&composer, 7, false);
    if (message_composer_insert_utf8(&composer, utf8_literal("alpha")) != MessageComposerResult::changed ||
        !message_composer_controller_commit_draft(&composer_controller).ok() ||
        !message_composer_controller_select_channel(&composer_controller, 8).ok() ||
        message_composer_insert_utf8(&composer, utf8_literal("beta")) != MessageComposerResult::changed ||
        !message_composer_controller_commit_draft(&composer_controller).ok()) {
        return 43;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kMessageDraftSwitchIterations; ++iteration) {
        const u64 channel_id = (iteration & 1u) == 0 ? 7 : 8;
        const MessageComposerControllerResult selected =
            message_composer_controller_select_channel(&composer_controller, channel_id);
        if (!selected.ok() || composer.channel_id != channel_id || composer.bytes == 0) {
            return 43;
        }
        checksum += composer.channel_id + composer.bytes + composer.caret;
    }
    const u64 message_draft_switch_us = clock_microseconds(clock, begin, clock_now());

    MessageComposerScene composer_scene{};
    UiWidget composer_widgets[kMessageComposerWidgetCapacity]{};
    UiLayoutNode composer_layout[kMessageComposerWidgetCapacity]{};
    if (!message_composer_scene_initialize(
            &composer_scene, Slice<UiWidget>{composer_widgets, kMessageComposerWidgetCapacity},
            Slice<UiLayoutNode>{composer_layout, kMessageComposerWidgetCapacity}) ||
        !message_composer_scene_update(&composer_scene, &composer, UiRect{300.0f, 700.0f, 1'000.0f, 780.0f},
                                       true)
             .ok()) {
        return 43;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kMessageComposerSelectionProjectionIterations; ++iteration) {
        const u32 position = (iteration & 1u) == 0 ? 0 : composer.bytes;
        const MessageComposerResult selected =
            message_composer_set_selection(&composer, position, position);
        const MessageComposerSceneResult projected =
            selected == MessageComposerResult::changed
                ? message_composer_scene_update(&composer_scene, &composer, composer_scene.viewport, true)
                : MessageComposerSceneResult{};
        if (selected != MessageComposerResult::changed || !projected.ok() || !projected.changed ||
            composer_scene.display_caret != position) {
            return 43;
        }
        checksum += composer_scene.display_caret + composer_scene.display_bytes;
    }
    const u64 message_composer_selection_projection_us = clock_microseconds(clock, begin, clock_now());

    constexpr u32 composer_layout_cached_iterations = 20'000;
    constexpr u32 composer_layout_rebuild_iterations = 1'000;
    if (message_composer_set_selection(&composer, 0, composer.bytes) != MessageComposerResult::changed ||
        !message_composer_scene_update(&composer_scene, &composer, composer_scene.viewport, true).ok()) {
        return 43;
    }
    TextSystem composer_text_system{};
    if (!text_system_initialize(&composer_text_system, L"Segoe UI", 14.0f).ok()) {
        return 43;
    }
    TextEditLayoutOwner composer_layout_owner{};
    TextEditMetric composer_layout_selections[kTextEditSelectionCapacity]{};
    composer_layout_owner.selections = composer_layout_selections;
    composer_layout_owner.selection_capacity = kTextEditSelectionCapacity;
    if (!text_composer_layout_measure(&composer_layout_owner, &composer_scene, composer_text_system) ||
        !composer_layout_owner.active || !composer_layout_owner.geometry ||
        composer_layout_owner.selection_count == 0 || composer_layout_owner.builds != 1) {
        text_edit_layout_release(&composer_layout_owner);
        text_system_release(&composer_text_system);
        return 43;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < composer_layout_cached_iterations; ++iteration) {
        if (!text_composer_layout_measure(&composer_layout_owner, &composer_scene,
                                            composer_text_system)) {
            text_edit_layout_release(&composer_layout_owner);
            text_system_release(&composer_text_system);
            return 43;
        }
        checksum += composer_layout_owner.caret.source_first + composer_layout_owner.selection_count;
    }
    const u64 composer_layout_cached_us = clock_microseconds(clock, begin, clock_now());
    begin = clock_now();
    for (u32 iteration = 0; iteration < composer_layout_rebuild_iterations; ++iteration) {
        text_edit_layout_invalidate(&composer_layout_owner);
        if (!text_composer_layout_measure(&composer_layout_owner, &composer_scene,
                                            composer_text_system)) {
            text_edit_layout_release(&composer_layout_owner);
            text_system_release(&composer_text_system);
            return 43;
        }
        checksum += composer_layout_owner.caret.source_first + composer_layout_owner.utf16_units;
    }
    const u64 composer_layout_rebuild_us = clock_microseconds(clock, begin, clock_now());
    const u32 composer_layout_builds = composer_layout_owner.builds;
    const u32 composer_layout_hits = composer_layout_owner.hits;

    char long_composer_source[kMessageComposerMaximumBytes]{};
    char long_composer_storage[kMessageComposerMaximumBytes]{};
    for (u32 index = 0; index < kMessageComposerMaximumBytes; ++index) {
        long_composer_source[index] = static_cast<char>('a' + index % 26u);
    }
    MessageComposer long_composer{};
    MessageComposerScene long_composer_scene{};
    UiWidget long_composer_widgets[kMessageComposerWidgetCapacity]{};
    UiLayoutNode long_composer_layout[kMessageComposerWidgetCapacity]{};
    if (!message_composer_initialize(&long_composer,
                                     Slice<char>{long_composer_storage, kMessageComposerMaximumBytes}) ||
        !message_composer_restore(&long_composer, 7,
                                  Utf8View{long_composer_source, kMessageComposerMaximumBytes},
                                  kMessageComposerMaximumBytes, kMessageComposerMaximumBytes) ||
        !message_composer_scene_initialize(
            &long_composer_scene, Slice<UiWidget>{long_composer_widgets, kMessageComposerWidgetCapacity},
            Slice<UiLayoutNode>{long_composer_layout, kMessageComposerWidgetCapacity}) ||
        !message_composer_scene_update(&long_composer_scene, &long_composer,
                                       UiRect{300.0f, 700.0f, 1'000.0f, 908.0f}, true)
             .ok()) {
        text_edit_layout_release(&composer_layout_owner);
        text_system_release(&composer_text_system);
        return 43;
    }
    text_edit_layout_invalidate(&composer_layout_owner);
    if (!text_composer_layout_measure(&composer_layout_owner, &long_composer_scene,
                                        composer_text_system) ||
        composer_layout_owner.utf16_units != kMessageComposerMaximumBytes ||
        composer_layout_owner.rejected != 0) {
        text_edit_layout_release(&composer_layout_owner);
        text_system_release(&composer_text_system);
        return 43;
    }
    checksum +=
        composer_layout_owner.utf16_units + composer_layout_owner.builds + composer_layout_owner.hits;
    text_edit_layout_release(&composer_layout_owner);
    text_system_release(&composer_text_system);

    begin = clock_now();
    for (u32 iteration = 0; iteration < kMessageComposerSendRoundTripIterations; ++iteration) {
        if (message_composer_insert_utf8(&composer, utf8_literal("send")) !=
            MessageComposerResult::changed) {
            return 43;
        }
        const u64 started = static_cast<u64>(iteration) * 3u + 1u;
        const MessageComposerControllerResult queued = message_composer_controller_queue_send(
            &composer_controller, true, 9, 1'725'012'496'900ull + iteration, started);
        MessageComposerSubmit submit;
        const MessageComposerControllerResult prepared =
            message_composer_controller_prepare_submit(&composer_controller, started + 1, &submit);
        const MessageComposerControllerResult marked =
            prepared.ok() ? message_composer_controller_mark_submitted(&composer_controller, submit.nonce)
                          : MessageComposerControllerResult{};
        const MessageComposerControllerResult http =
            marked.ok() ? message_composer_controller_observe_http(
                              &composer_controller, 200, MessageSendHttpDisposition::success, started + 2)
                        : MessageComposerControllerResult{};
        const MessageSendObserveResult gateway =
            http.ok() ? message_send_tracker_observe_gateway(
                            message_composer_controller_send_tracker(&composer_controller), &send_timeline,
                            queued.nonce, composer.channel_id, 9, 1'000'000 + iteration, started + 3)
                      : MessageSendObserveResult{};
        MessageSendCompletion completed{};
        if (!queued.ok() || !prepared.ok() || !marked.ok() || !http.ok() || !gateway.ok() ||
            !gateway.completed ||
            !message_composer_controller_take_completion(&composer_controller, &completed) ||
            completed.kind != MessageSendCompletionKind::accepted ||
            composer_controller.request_count != 0 || send_store.count != 0) {
            return 43;
        }
        checksum += completed.nonce + completed.authoritative_id;
    }
    const u64 message_composer_send_round_trip_us = clock_microseconds(clock, begin, clock_now());

    if (!ui_variable_list_initialize(&message_list, Slice<u32>{g_message_scene_heights, kMessageSceneItems},
                                     Slice<u64>{g_message_scene_fenwick, kMessageSceneItems},
                                     kMessageSceneItems, message_default_units) ||
        !message_timeline_initialize(&message_timeline, &message_store, &message_list,
                                     message_default_units, ui_extent_units(720.0f), false)) {
        return 32;
    }
    MessageScene message_scene{};
    if (!message_scene_initialize(
            &message_scene, &message_timeline, Slice<MessageSceneRowWidget>{g_message_scene_rows, 32},
            Slice<UiWidget>{g_message_scene_widgets, 32 * kMessageSceneWidgetsPerRow + 1},
            Slice<UiLayoutNode>{g_message_scene_layout, 32 * kMessageSceneWidgetsPerRow + 1}) ||
        !message_scene_update(&message_scene, message_viewport).ok()) {
        return 32;
    }

    MessageStore rich_message_store{};
    MessageRichStore rich_message_store_detail{};
    UiVariableList rich_message_list{};
    MessageTimeline rich_message_timeline{};
    MessageScene rich_message_scene{};
    GatewayMessageReaction rich_reactions[5]{};
    GatewayMessageAttachment rich_attachments[2]{};
    GatewayMessageEmbed rich_embeds[2]{};
    GatewayMessageSticker rich_stickers[2]{};
    GatewayMessageComponent rich_components[4]{};
    for (u32 index = 0; index < 5; ++index) {
        rich_reactions[index].emoji_id = 700 + index;
        rich_reactions[index].name = utf8_literal("party");
        rich_reactions[index].count = index + 1;
        rich_reactions[index].normal_count = index + 1;
    }
    for (u32 index = 0; index < 2; ++index) {
        rich_attachments[index].id = 800 + index;
        rich_attachments[index].filename = utf8_literal("preview.jpg");
        rich_attachments[index].content_type = utf8_literal("image/jpeg");
        rich_attachments[index].width = 320;
        rich_attachments[index].height = 180;
        rich_attachments[index].has_width = true;
        rich_attachments[index].has_height = true;
        rich_embeds[index].title = utf8_literal("Retained embed");
        rich_embeds[index].description = utf8_literal("Measured rich message layout benchmark");
        rich_embeds[index].type = GatewayMessageEmbedType::rich;
        rich_stickers[index].id = 900 + index;
        rich_stickers[index].name = utf8_literal("Sticker");
        rich_stickers[index].format_type = 2;
    }
    for (u32 index = 0; index < 4; ++index) {
        rich_components[index].content = utf8_literal("Component content");
        rich_components[index].label = utf8_literal("Action");
        rich_components[index].type = 2;
        rich_components[index].parent = 0xFFFFu;
    }
    GatewayMessageEvent rich_message_event{};
    rich_message_event.id = 900'000;
    rich_message_event.fields = gateway_message_field_reactions | gateway_message_field_attachments |
                                gateway_message_field_embeds | gateway_message_field_sticker_items |
                                gateway_message_field_components;
    rich_message_event.reactions = ConstSlice<GatewayMessageReaction>{rich_reactions, 5};
    rich_message_event.attachments = ConstSlice<GatewayMessageAttachment>{rich_attachments, 2};
    rich_message_event.embeds = ConstSlice<GatewayMessageEmbed>{rich_embeds, 2};
    rich_message_event.stickers = ConstSlice<GatewayMessageSticker>{rich_stickers, 2};
    rich_message_event.components = ConstSlice<GatewayMessageComponent>{rich_components, 4};
    rich_message_event.rich_truncations = gateway_message_rich_truncated_text;
    if (!message_store_initialize(&rich_message_store, Slice<MessageRecord>{g_rich_message_records, 1},
                                  Slice<u32>{g_rich_message_order, 1},
                                  Slice<u8>{g_rich_message_text, sizeof(g_rich_message_text)}) ||
        !message_store_upsert(&rich_message_store, MessageInput{900'000, 7, 9, 1'700'000'000'000ull, 0,
                                                                utf8_literal("rich benchmark"), 0})
             .ok() ||
        !message_rich_store_initialize(
            &rich_message_store_detail, Slice<MessageRichRecord>{g_rich_message_records_detail, 1},
            Slice<u16>{g_rich_message_order_detail, 1},
            Slice<u8>{g_rich_message_text_detail, sizeof(g_rich_message_text_detail)}) ||
        !message_rich_store_apply(&rich_message_store_detail, 900'000, 1, rich_message_event).ok() ||
        !ui_variable_list_initialize(&rich_message_list, Slice<u32>{g_rich_message_heights, 1},
                                     Slice<u64>{g_rich_message_fenwick, 1}, 1, message_default_units) ||
        !message_timeline_initialize(&rich_message_timeline, &rich_message_store, &rich_message_list,
                                     message_default_units, ui_extent_units(720.0f), true) ||
        !message_timeline_bind_rich_store(&rich_message_timeline, &rich_message_store_detail) ||
        !message_scene_initialize(
            &rich_message_scene, &rich_message_timeline,
            Slice<MessageSceneRowWidget>{g_rich_message_rows, 1},
            Slice<UiWidget>{g_rich_message_widgets, kMessageSceneWidgetsPerRow + 1},
            Slice<UiLayoutNode>{g_rich_message_layout, kMessageSceneWidgetsPerRow + 1}) ||
        !message_scene_update(&rich_message_scene, message_viewport).ok()) {
        return 37;
    }
    const UiRect rich_message_viewports[2]{message_viewport, UiRect{0.0f, 0.0f, 360.0f, 720.0f}};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kRichMessageLayoutIterations; ++iteration) {
        const MessageSceneResult updated =
            message_scene_update(&rich_message_scene, rich_message_viewports[iteration & 1u]);
        if (!updated.ok()) {
            return 37;
        }
        checksum += updated.count + rich_message_scene.widgets.count +
                    rich_message_scene.rows[0].component_item_count;
    }
    const u64 rich_message_layout_us = clock_microseconds(clock, begin, clock_now());

    const MessageRichRecord* materialized_rich =
        message_rich_store_find(rich_message_store_detail, 900'000);
    MessageSceneRowWidget materialized_row_template{};
    materialized_row_template.rich_item_layout =
        message_rich_estimate_item_layout(&rich_message_store_detail, materialized_rich, 660.0f);
    if (materialized_rich) {
        materialized_row_template.rich_layout = message_rich_aggregate_layout(
            *materialized_rich, materialized_row_template.rich_item_layout, 660.0f);
    }
    UiWidgetManager rich_widget_manager{};
    UiLayoutStyle rich_widget_root_style = ui_layout_style();
    rich_widget_root_style.flow = UiLayoutFlow::vertical;
    rich_widget_root_style.width = ui_layout_fill();
    rich_widget_root_style.height = ui_layout_fill();
    rich_widget_root_style.horizontal_alignment = UiLayoutAlignment::stretch;
    if (!materialized_rich ||
        !ui_widget_manager_initialize(
            &rich_widget_manager, Slice<UiWidget>{g_rich_message_widgets, kMessageSceneWidgetsPerRow + 1},
            Slice<UiLayoutNode>{g_rich_message_layout, kMessageSceneWidgetsPerRow + 1})) {
        return 38;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kRichMessageMaterializeIterations; ++iteration) {
        ui_widget_manager_reset(&rich_widget_manager);
        MessageSceneRowWidget materialized_row = materialized_row_template;
        const UiWidgetResult root = ui_widget_manager_add(
            &rich_widget_manager, ui_widget_invalid_handle(),
            UiWidgetDesc{1, UiWidgetKind::container, ui_widget_enabled,
                         UiLayoutNodeSpec{utf8_literal("RichMaterializationRoot"), rich_widget_root_style,
                                          UiSize{}, 0.0f, 1}});
        if (!root.ok() ||
            !message_rich_widgets_build(&rich_widget_manager, root.handle, 2, materialized_rich,
                                        &materialized_row) ||
            !message_rich_widgets_apply(&rich_widget_manager, materialized_rich, &materialized_row) ||
            !ui_widget_manager_layout_rectangle(&rich_widget_manager, UiRect{0.0f, 0.0f, 728.0f, 720.0f})
                 .ok()) {
            return 38;
        }
        checksum += rich_widget_manager.count + materialized_row.component_item_count +
                    materialized_row.reaction_button_count;
    }
    const u64 rich_message_materialize_us = clock_microseconds(clock, begin, clock_now());

    const u64 maximum_message_scroll = message_list.total_units - ui_extent_units(720.0f);
    begin = clock_now();
    for (u32 iteration = 0; iteration < kMessageSceneIterations; ++iteration) {
        const u64 scroll = (static_cast<u64>(iteration) * 9'973ull) % (maximum_message_scroll + 1u);
        if (!message_timeline_set_scroll(&message_timeline, scroll, false)) {
            return 32;
        }
        const MessageSceneResult updated = message_scene_update(&message_scene, message_viewport);
        if (!updated.ok()) {
            return 32;
        }
        checksum += updated.first + updated.count + message_scene.widgets.count;
    }
    const u64 message_scene_layout_us = clock_microseconds(clock, begin, clock_now());

    PaintStream message_paint{};
    MessageMediaCommand message_media_commands[1]{};
    MessageMediaCommandStream message_media_stream{};
    if (!message_media_command_stream_initialize(&message_media_stream,
                                                 Slice<MessageMediaCommand>{message_media_commands, 1})) {
        return 33;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kPresentationIterations; ++iteration) {
        paint_stream_reset(&message_paint, message_viewport.right, message_viewport.bottom);
        MessagePresentationResult presented{};
        if (!message_presentation_build(message_scene, kDefaultTheme, &message_paint, &message_media_stream,
                                        &presented)) {
            return 33;
        }
        checksum += presented.geometry_quads + message_paint.batch_count + message_scene.count;
    }
    const u64 message_presentation_us = clock_microseconds(clock, begin, clock_now());

    UiTextFlowLayout text_flow{};
    UiRect text_flow_item{};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kTextFlowIterations; ++iteration) {
        if (!ui_text_flow_initialize(&text_flow, UiRect{0.0f, 0.0f, 640.0f, 512.0f},
                                     UiRect{0.0f, 0.0f, 640.0f, 512.0f})) {
            return 36;
        }
        for (u32 item = 0; item < 8; ++item) {
            const float inset = static_cast<float>((iteration + item) & 3u);
            const float measured = 16.0f + static_cast<float>((iteration + item) & 3u) * 8.0f;
            if (!ui_text_flow_begin_item(&text_flow, inset, inset, &text_flow_item) ||
                !ui_text_flow_commit_item(&text_flow, measured, 16.0f, 3.0f)) {
                return 36;
            }
        }
        checksum += static_cast<u64>(ui_text_flow_required_height(text_flow)) + text_flow.item_count +
                    static_cast<u32>(text_flow_item.left);
    }
    const u64 text_flow_us = clock_microseconds(clock, begin, clock_now());

    MessageTextPresentation message_text{};
    if (!message_text_presentation_initialize(
            &message_text, Slice<MessageTextCommand>{g_message_text_commands, kMaximumMessageTextCommands},
            Slice<char>{g_message_text_utf8, kMaximumMessageTextUtf8Bytes},
            Slice<TextStyleSpan>{g_message_text_styles, kMaximumMessageTextStyleSpans},
            Slice<MessageMarkdownSemantic>{g_message_text_semantics, kMaximumMessageTextSemantics},
            Slice<MessageTextMeasurement>{g_message_text_measurements, kMaximumMessageTextMeasurements},
            Slice<MessageTextSpoilerReveal>{g_message_text_spoiler_reveals,
                                            kMaximumMessageTextSpoilerReveals},
            Slice<u32>{g_message_text_byte_styles, kMessageTextByteStyleScratchCapacity})) {
        return 35;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kMessageSceneIterations; ++iteration) {
        const MessageTextPresentationResult projected = message_text_presentation_build(
            &message_text, message_scene, state, nullptr, navigation, kDefaultTheme, 1'700'000'100ull);
        if (!projected.ok()) {
            return 35;
        }
        checksum += projected.command_count + projected.measurement_count;
    }
    const u64 message_text_presentation_us = clock_microseconds(clock, begin, clock_now());

    BenchmarkTextMeasureState text_measure_state{};
    UiTextMeasureCache text_measure_cache{};
    if (!ui_text_measure_cache_initialize(
            &text_measure_cache,
            Slice<UiTextMeasureRecord>{g_text_measure_records, kMaximumMessageTextMeasureCacheRecords},
            benchmark_text_measure_backend, &text_measure_state)) {
        return 39;
    }
    const UiTextMeasureRequest cached_measure_request{UiTextMeasureKey{1, 0x4d6169436f7264ull, 1, 0, 1},
                                                      utf8_literal("Retained text measurement"),
                                                      ConstSlice<TextStyleSpan>{}, UiSize{640.0f, 720.0f}};
    UiTextMeasureValue cached_measure_value{};
    if (!ui_text_measure_cache_measure(&text_measure_cache, cached_measure_request, &cached_measure_value)
             .ok()) {
        return 39;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kTextMeasureCacheHitIterations; ++iteration) {
        if (!ui_text_measure_cache_measure(&text_measure_cache, cached_measure_request,
                                           &cached_measure_value)
                 .ok()) {
            return 39;
        }
        checksum += cached_measure_value.line_count;
    }
    const u64 text_measure_cache_hit_us = clock_microseconds(clock, begin, clock_now());

    if (!ui_text_measure_cache_initialize(
            &text_measure_cache,
            Slice<UiTextMeasureRecord>{g_text_measure_records, kMaximumMessageTextMeasureCacheRecords},
            benchmark_text_measure_backend, &text_measure_state)) {
        return 39;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kTextMeasureCacheMissIterations; ++iteration) {
        const UiTextMeasureRequest request{
            UiTextMeasureKey{static_cast<u64>(iteration) + 1u,
                             0x9e3779b97f4a7c15ull * (static_cast<u64>(iteration) + 1u), 1,
                             static_cast<u16>(iteration), 1},
            cached_measure_request.text, ConstSlice<TextStyleSpan>{}, UiSize{640.0f, 720.0f}};
        if (!ui_text_measure_cache_measure(&text_measure_cache, request, &cached_measure_value).ok()) {
            return 39;
        }
        checksum += cached_measure_value.line_count;
    }
    const u64 text_measure_cache_miss_us = clock_microseconds(clock, begin, clock_now());

    if (!ui_text_measure_cache_initialize(
            &text_measure_cache,
            Slice<UiTextMeasureRecord>{g_text_measure_records, kMaximumMessageTextMeasureCacheRecords},
            benchmark_text_measure_backend, &text_measure_state)) {
        return 40;
    }
    bool measurements_changed = false;
    if (!message_text_presentation_measure(&message_text, &text_measure_cache, 1, &measurements_changed)) {
        return 40;
    }
    const u32 prewarmed_backend_calls = text_measure_cache.backend_calls;
    if (!message_text_presentation_measure(&message_text, &text_measure_cache, 1, &measurements_changed) ||
        measurements_changed || text_measure_cache.backend_calls != prewarmed_backend_calls) {
        return 40;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kMessageTextMeasureIterations; ++iteration) {
        if (!message_text_presentation_measure(&message_text, &text_measure_cache, 1,
                                               &measurements_changed) ||
            measurements_changed) {
            return 40;
        }
        checksum += message_text.command_count + message_text.measurement_count;
    }
    const u64 message_text_measure_cached_us = clock_microseconds(clock, begin, clock_now());
    if (text_measure_cache.backend_calls != prewarmed_backend_calls) {
        return 40;
    }

    if (!ui_text_measure_cache_initialize(
            &text_measure_cache,
            Slice<UiTextMeasureRecord>{g_text_measure_records, kMaximumMessageTextMeasureCacheRecords},
            benchmark_text_measure_backend, &text_measure_state)) {
        return 41;
    }
    BenchmarkTextFinalizeState text_finalize_state{};
    const MessageTextCoordinatorRequest coordinator_request{&message_scene,
                                                            &message_text,
                                                            &text_measure_cache,
                                                            &state,
                                                            nullptr,
                                                            &navigation,
                                                            &kDefaultTheme,
                                                            benchmark_text_finalize,
                                                            &text_finalize_state,
                                                            1'700'000'100ull,
                                                            1};
    const MessageTextCoordinatorResult coordinator_prewarm =
        message_text_coordinator_converge(coordinator_request);
    const u32 coordinator_backend_calls = text_measure_cache.backend_calls;
    const MessageTextCoordinatorResult coordinator_stable =
        coordinator_prewarm.ok() ? message_text_coordinator_converge(coordinator_request)
                                 : MessageTextCoordinatorResult{};
    if (!coordinator_prewarm.ok() || !coordinator_prewarm.finalized || !coordinator_stable.ok() ||
        !coordinator_stable.finalized || coordinator_stable.passes != 1 ||
        text_measure_cache.backend_calls != coordinator_backend_calls || text_finalize_state.calls != 2) {
        return 41;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kMessageTextConvergenceIterations; ++iteration) {
        const MessageTextCoordinatorResult converged =
            message_text_coordinator_converge(coordinator_request);
        if (!converged.ok() || !converged.finalized || converged.passes != 1) {
            return 41;
        }
        checksum += converged.passes + message_text.command_count + message_text.measurement_count;
    }
    const u64 message_text_converge_cached_us = clock_microseconds(clock, begin, clock_now());
    if (text_measure_cache.backend_calls != coordinator_backend_calls ||
        text_finalize_state.calls != static_cast<u64>(kMessageTextConvergenceIterations) + 2u) {
        return 41;
    }
    constexpr u32 content_text_retention_hit_iterations = 500'000;
    constexpr u32 content_text_retention_miss_iterations = 50'000;
    ContentTextRetentionOwner content_text_retention{};
    ContentTextRetentionKey content_text_key{
        1,
        message_text.projection_signature,
        0,
        content_text_theme_signature(kDefaultTheme),
        0,
        0,
        0,
        0,
        0,
        0,
        96,
        0,
        0,
        true,
    };
    content_text_retention_commit(&content_text_retention, content_text_key);
    begin = clock_now();
    for (u32 iteration = 0; iteration < content_text_retention_hit_iterations; ++iteration) {
        if (!content_text_retention_probe(&content_text_retention, content_text_key)) {
            return 41;
        }
        checksum += content_text_retention.hits;
    }
    const u64 content_text_retention_hit_us = clock_microseconds(clock, begin, clock_now());
    begin = clock_now();
    for (u32 iteration = 0; iteration < content_text_retention_miss_iterations; ++iteration) {
        content_text_key.atlas_evicted_entries = iteration + 1u;
        if (content_text_retention_probe(&content_text_retention, content_text_key)) {
            return 41;
        }
        content_text_retention_commit(&content_text_retention, content_text_key);
        checksum += content_text_retention.builds;
    }
    const u64 content_text_retention_miss_us = clock_microseconds(clock, begin, clock_now());
    g_ui_checksum = checksum;
    if (layout_us == 0 || layout_reflow_us == 0 || friends_reflow_us == 0 || constrained_reflow_us == 0 ||
        content_bounds_validation_us == 0 || login_layout_us == 0 || login_mfa_layout_us == 0 ||
        login_cached_layout_us == 0 || login_reflow_us == 0 || login_presentation_us == 0 ||
        login_text_presentation_us == 0 || layout_validation_us == 0 || qr_us == 0 || secure_edit_us == 0 ||
        secure_edit_render_us == 0 || secure_edit_acp_us == 0 || secure_text_store_us == 0 ||
        member_list_us == 0 || variable_us == 0 || projection_us == 0 ||
        guild_rail_layout_us == 0 || shell_navigation_layout_us == 0 || shell_presentation_us == 0 ||
        shell_text_presentation_us == 0 || message_scene_layout_us == 0 || rich_message_layout_us == 0 ||
        rich_message_materialize_us == 0 || message_presentation_us == 0 || text_flow_us == 0 ||
        wrap_layout_us == 0 || message_text_presentation_us == 0 || text_measure_cache_hit_us == 0 ||
        text_measure_cache_miss_us == 0 || message_text_measure_cached_us == 0 ||
        message_text_converge_cached_us == 0 || content_text_retention_hit_us == 0 ||
        content_text_retention_miss_us == 0 ||
        content_text_retention.builds != content_text_retention_miss_iterations + 1u ||
        content_text_retention.hits != content_text_retention_hit_iterations ||
        content_text_retention.misses != content_text_retention_miss_iterations ||
        message_view_runtime_initialize_us == 0 || message_view_runtime_reindex_us == 0 ||
        message_send_round_trip_us == 0 || message_draft_switch_us == 0 ||
        message_composer_selection_projection_us == 0 || message_composer_send_round_trip_us == 0 ||
        message_media_legacy_resolve_us == 0 || message_media_snapshot_us == 0 ||
        composer_layout_cached_us == 0 || composer_layout_rebuild_us == 0 ||
        composer_layout_builds != composer_layout_rebuild_iterations + 1u ||
        composer_layout_hits != composer_layout_cached_iterations || g_ui_checksum == 0) {
        return 11;
    }
    write_runs("ui_scene_layout", kLayoutIterations, layout_us);
    write_runs("ui_scene_reflow", kLayoutReflowIterations, layout_reflow_us);
    write_runs("ui_friends_scene_reflow", kLayoutReflowIterations, friends_reflow_us);
    write_runs("ui_constrained_text_reflow", kLayoutReflowIterations, constrained_reflow_us);
    write_runs("ui_content_bounds_validation", kContentBoundsValidationIterations, content_bounds_validation_us);
    write_runs("ui_login_layout", kLoginLayoutIterations, login_layout_us);
    write_runs("ui_login_mfa_layout", kLoginLayoutIterations, login_mfa_layout_us);
    write_runs("ui_login_layout_cached", kLoginLayoutIterations, login_cached_layout_us);
    write_runs("ui_login_reflow", kLoginLayoutIterations, login_reflow_us);
    write_runs("ui_login_presentation", kPresentationIterations, login_presentation_us);
    write_runs("ui_login_text_presentation", kPresentationIterations, login_text_presentation_us);
    write_runs("ui_layout_validation", kLayoutValidationIterations, layout_validation_us);
    write_runs("ui_qr_v5_m_encode", kQrIterations, qr_us);
    write_runs("ui_secure_edit", kSecureEditIterations, secure_edit_us);
    write_runs("ui_secure_edit_render_window", kSecureEditIterations, secure_edit_render_us);
    write_runs("ui_secure_edit_utf16_acp", kSecureEditIterations, secure_edit_acp_us);
    write_runs("ui_secure_text_store_read_lock", kSecureTextStoreIterations, secure_text_store_us);
    write_runs("ui_member_list_100k_scroll_materialize", kMemberListIterations, member_list_us);
    write_runs("ui_variable_range", kVariableIterations, variable_us);
    write_runs("ui_navigation_projection", kProjectionIterations, projection_us);
    write_runs("ui_guild_rail_widget_layout", kPresentationIterations, guild_rail_layout_us);
    write_runs("ui_shell_navigation_widget_layout", kPresentationIterations, shell_navigation_layout_us);
    write_runs("ui_shell_presentation", kPresentationIterations, shell_presentation_us);
    write_runs("ui_shell_text_presentation", kPresentationIterations, shell_text_presentation_us);
    write_runs("ui_message_scene_scroll_layout", kMessageSceneIterations, message_scene_layout_us);
    write_runs("ui_rich_message_widget_layout", kRichMessageLayoutIterations, rich_message_layout_us);
    write_runs("ui_rich_message_widget_materialize", kRichMessageMaterializeIterations, rich_message_materialize_us);
    write_runs("ui_message_presentation", kPresentationIterations, message_presentation_us);
    write_runs("ui_text_flow_layout", kTextFlowIterations, text_flow_us);
    write_runs("ui_wrap_widget_layout", kWrapLayoutIterations, wrap_layout_us);
    write_runs("ui_message_text_presentation", kMessageSceneIterations, message_text_presentation_us);
    write_runs("ui_text_measure_cache_hit", kTextMeasureCacheHitIterations, text_measure_cache_hit_us);
    write_runs("ui_text_measure_cache_miss", kTextMeasureCacheMissIterations, text_measure_cache_miss_us);
    write_runs("ui_message_text_measure_cached", kMessageTextMeasureIterations, message_text_measure_cached_us);
    write_runs("ui_message_text_converge_cached", kMessageTextConvergenceIterations, message_text_converge_cached_us);
    write_runs("ui_content_text_retention_hit", content_text_retention_hit_iterations, content_text_retention_hit_us);
    write_runs("ui_content_text_retention_miss_commit", content_text_retention_miss_iterations,
               content_text_retention_miss_us);
    write_runs("ui_message_view_runtime_initialize", kMessageViewRuntimeInitializeIterations,
               message_view_runtime_initialize_us);
    write_runs("ui_message_view_runtime_reindex", kMessageViewRuntimeReindexIterations,
               message_view_runtime_reindex_us);
    write_runs("ui_message_send_round_trip", kMessageSendRoundTripIterations, message_send_round_trip_us);
    write_runs("ui_message_draft_switch", kMessageDraftSwitchIterations, message_draft_switch_us);
    write_runs("ui_message_composer_selection_projection", kMessageComposerSelectionProjectionIterations,
               message_composer_selection_projection_us);
    write_runs("ui_composer_layout_cached", composer_layout_cached_iterations, composer_layout_cached_us);
    write_runs("ui_composer_layout_rebuild", composer_layout_rebuild_iterations, composer_layout_rebuild_us);
    write_runs("ui_message_composer_send_round_trip", kMessageComposerSendRoundTripIterations,
               message_composer_send_round_trip_us);
    write_runs("ui_message_media_legacy_double_resolve_192", kMessageMediaSnapshotIterations,
               message_media_legacy_resolve_us);
    write_runs("ui_message_media_single_snapshot_192", kMessageMediaSnapshotIterations, message_media_snapshot_us);
    write_metric("ui_widget_size", sizeof(UiWidget), "bytes");
    write_metric("ui_layout_node_size", sizeof(UiLayoutNode), "bytes");
    write_metric("ui_message_scene_row_size", sizeof(MessageSceneRowWidget), "bytes");
    write_metric("ui_message_scene_source_items", kMessageSceneItems, "items");
    write_metric("ui_message_scene_retained_rows", 32, "rows");
    write_metric("ui_message_scene_retained_nodes", 32 * kMessageSceneWidgetsPerRow + 1, "nodes");
    write_metric("ui_member_list_source_items", kMemberListItems, "items");
    write_metric("ui_member_list_retained_nodes", 1 + 32 * kMemberListWidgetsPerRow, "nodes");
    write_metric("ui_message_text_command_size", sizeof(MessageTextCommand), "bytes");
    write_metric("ui_message_text_measurement_size", sizeof(MessageTextMeasurement), "bytes");
    write_metric("ui_text_measure_record_size", sizeof(UiTextMeasureRecord), "bytes");
    write_metric("ui_message_text_measure_cache_capacity",
                 sizeof(UiTextMeasureRecord) * static_cast<u64>(kMaximumMessageTextMeasureCacheRecords), "bytes");
    write_metric("ui_message_send_tracker_size", sizeof(MessageSendTracker), "bytes");
    write_metric("ui_message_draft_store_size", sizeof(MessageDraftStore), "bytes");
    write_metric("ui_message_draft_record_size", sizeof(MessageDraftRecord), "bytes");
    write_metric("ui_message_composer_controller_size", sizeof(MessageComposerController), "bytes");
    write_metric("ui_message_composer_scene_size", sizeof(MessageComposerScene), "bytes");
    write_metric("ui_message_media_controller_size", sizeof(MessageMediaController), "bytes");
    write_metric("ui_message_media_benchmark_backing",
                 sizeof(g_message_media_visible_rows) + sizeof(g_message_media_asset_marks), "bytes");
    write_metric("ui_d3d11_device_owner_size", sizeof(D3D11DeviceOwner), "bytes");
    write_metric("ui_d3d11_draw_pipeline_owner_size", sizeof(D3D11DrawPipelineOwner), "bytes");
    write_metric("ui_d3d11_gpu_timer_owner_size", sizeof(D3D11GpuTimerOwner), "bytes");
    write_metric("ui_text_composer_layout_owner_size", sizeof(TextEditLayoutOwner), "bytes");
    write_metric("ui_message_hit_owner_size", sizeof(MessageHitTestCacheOwner), "bytes");
    write_metric("ui_d3d11_glyph_text_owner_size", sizeof(D3D11GlyphTextOwner), "bytes");
    write_metric("ui_text_command_stream_size", sizeof(UiTextCommandStream), "bytes");
    write_metric("ui_message_text_presentation_size", sizeof(MessageTextPresentation), "bytes");
    write_metric("ui_content_text_retention_owner_size", sizeof(ContentTextRetentionOwner), "bytes");
    write_metric("ui_d3d11_renderer_size", sizeof(D3D11Renderer), "bytes");
    write_metric("ui_message_scene_retained_capacity_per_row",
                 sizeof(MessageSceneRowWidget) + static_cast<u64>(kMessageSceneWidgetsPerRow) *
                                                     (sizeof(UiWidget) + sizeof(UiLayoutNode)),
                 "bytes");
    login_scene_release(&login_scene);
    if (!benchmark_widget_scroll(clock, write_metric)) return 43;
    if (!benchmark_input_router(clock, write_metric)) return 44;
    if (!benchmark_navigation_shortcuts(clock, write_metric)) return 45;
    if (!benchmark_private_channels(clock, write_metric)) return 46;
    if (!benchmark_text_entry(clock, write_metric)) return 47;
    if (!benchmark_text_form(clock, write_metric)) return 48;
    ui_scene_release(&friends_scene);
    ui_scene_release(&scene);
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_ui_benchmark_entry() noexcept {
    const int result = maicord::run();
    if (result != 0) maicord::write_metric("ui_benchmark_failure_stage", static_cast<maicord::u64>(result), "stage");
    ExitProcess(static_cast<UINT>(result));
}
