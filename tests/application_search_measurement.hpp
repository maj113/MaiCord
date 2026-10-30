#pragma once

// Included after application_search.cpp in its isolated runtime executable.
// No production hook or permanent renderer/text owner is introduced.
namespace maicord::search_runtime_test {
void check(bool, const char*) noexcept;
inline TextSystem measurement_text_owner{};
inline bool measurement_text_active{};
inline u32 measurement_text_requests{};
inline TextSystem search_measurement_text_system() noexcept {
    if (!measurement_text_active)
        return {};
    ++measurement_text_requests;
    return measurement_text_owner;
}

inline void test_search_measurement() noexcept {
    using namespace app_detail;
    ApplicationSearch search{};
    auto* bank = static_cast<SearchBank*>(
        VirtualAlloc(nullptr, sizeof(SearchBank), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    struct Release {
        ApplicationSearch& search;
        SearchBank* bank;
        u32 dpi;
        ~Release() noexcept {
            measurement_text_active = false;
            text_system_release(&measurement_text_owner);
            ui_search_scene_release(&search.scene);
            if (bank)
                VirtualFree(bank, 0, MEM_RELEASE);
            g_app.renderer.dpi = dpi;
        }
    } release{search, bank, g_app.renderer.dpi};
    const bool initialized =
        bank && text_system_initialize(&measurement_text_owner, L"Segoe UI", 16).ok();
    check(initialized, "search measurement: real native text owner and bounded result bank initialize");
    if (!initialized)
        return;
    measurement_text_active = true;
    search.bank = bank;
    search.panel = true;
    search.content.edit.active = true;
    search.source = {&search, 1, 1, app_detail::resolve};
    search.content.panel = true;
    search.content.phase = UiSearchPhase::ready;
    search.content.revision = 1;
    bank->page.group_count = bank->page.message_count = 1;
    bank->groups[0] = {0, 1, 1};
    auto& message = bank->messages[0];
    message.message.id = 301;
    message.message.channel_id = 70;
    message.message.author_name = utf8_literal("Original native author");
    message.hit = true;
    const auto long_body = utf8_literal(
        "Native line one\nNative line two\nNative line three\nNative line four\nNative line five");
    const auto short_body = utf8_literal("Native short line");
    const UiRect viewport{72, 72, 1280, 680}, title{0, 0, 1280, 48};
    const auto publish = [&](Utf8View body) noexcept {
        message.message.content = body;
        decorate(*bank);
        ++search.source.revision;
        return ui_search_scene_update(&search.scene, &search.source, &search.content, viewport, title) &&
               app_detail::measure_rows(search) &&
               ui_search_scene_update(&search.scene, &search.source, &search.content, viewport, title);
    };
    check(publish(long_body),
          "search measurement: accepted original multiline body measures through the actual host helper");
    if (!search.scene.count || !bank->measured_native_text)
        return;
    const float long_height = bank->body_heights[0];
    UiTextCommand commands[64]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    bool body_geometry = false;
    if (ui_text_command_stream_initialize(&stream, {commands, 64}) &&
        ui_search_scene_text_output(search.scene, kDefaultTheme, &output, nullptr))
        for (u32 i = 0; i < stream.count; ++i)
            if (utf8_equal(commands[i].text, long_body))
                body_geometry = ui_rect_width(commands[i].rectangle) == bank->measured_width &&
                                ui_rect_height(commands[i].rectangle) == long_height;
    check(body_geometry,
          "search measurement: host width and height agree with the actual native body text rectangle");
    wchar_t utf16[128]{};
    const auto metrics = text_measure_utf8(&measurement_text_owner, long_body, {}, bank->measured_width, 464,
                                           {utf16, 128}, TextWrapping::wrap);
    const float difference = long_height - (metrics.height + 2);
    check(metrics.ok() && metrics.line_count == 5 && difference > -1 && difference < 1 && long_height > 48,
          "search measurement: the five-line body's native metrics fit the source body extent with its padding");
    const auto* widgets = search.scene.widgets.storage.data;
    const u16 generation = search.scene.widgets.generation;
    const u32 revision = search.source.revision;
    const float extent = search.scene.row_offsets[1];
    MEMORY_BASIC_INFORMATION committed_before{}, committed_after{};
    const bool before = VirtualQuery(bank, &committed_before, sizeof(committed_before)) != 0;
    const u32 requests = measurement_text_requests;
    bool stable = true;
    for (u32 i = 0; i < 64; ++i)
        stable &= !app_detail::measure_rows(search) &&
                  ui_search_scene_update(&search.scene, &search.source, &search.content, viewport, title) &&
                  search.source.revision == revision && bank->body_heights[0] == long_height &&
                  search.scene.row_offsets[1] == extent && search.scene.widgets.storage.data == widgets &&
                  search.scene.widgets.generation == generation;
    const bool after = VirtualQuery(bank, &committed_after, sizeof(committed_after)) != 0;
    check(stable && measurement_text_requests == requests + 64 && before && after &&
              committed_before.BaseAddress == committed_after.BaseAddress &&
              committed_before.RegionSize == committed_after.RegionSize,
          "search measurement: 64 unchanged host passes return before measurement and retain rows, widgets "
          "and committed bank storage");
    check(publish(short_body) && bank->body_heights[0] < long_height && publish(long_body) &&
              bank->body_heights[0] == long_height,
          "search measurement: same-bank A-to-B-to-A accepted publications invalidate cached heights "
          "before reuse");
    g_app.renderer.dpi = 144;
    const u32 dpi_revision = search.source.revision;
    check(app_detail::measure_rows(search) && bank->measured_dpi == 144 &&
              search.source.revision == dpi_revision + 1 && !app_detail::measure_rows(search),
          "search measurement: DPI changes rebuild once then resume the cache fast path");
    measurement_text_owner.tracking = .25f;
    check(app_detail::measure_rows(search) && bank->measured_tracking == .25f &&
              !app_detail::measure_rows(search),
          "search measurement: copied text tracking changes rebuild once rather than reuse stale metrics");
    search.panel = false;
    const u32 dormant_revision = search.source.revision, dormant_requests = measurement_text_requests;
    check(!app_detail::measure_rows(search) && search.source.revision == dormant_revision &&
              measurement_text_requests == dormant_requests,
          "search measurement: a closed results pane admits no text system lookup or measurement");
}
} // namespace maicord::search_runtime_test
