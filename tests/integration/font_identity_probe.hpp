#pragma once

namespace maicord {
// A separate renderer keeps deliberate identity churn out of screenshot/cache
// measurements. Each simulation of one face is a distinct renderer face ID;
// an equal run reuses its ID and a run from another session is rejected.
inline bool probe_renderer_font_aliases(D3D11Renderer& renderer) noexcept {
    D3D11GlyphTextOwner owner{};
    struct ReleaseOwner { D3D11GlyphTextOwner& o; ~ReleaseOwner() noexcept { d3d11_glyph_text_release(&o); } } release_owner{owner};
    if (!d3d11_glyph_text_initialize(&owner, renderer.graphics.device,
            renderer.graphics.context, renderer.dpi).ok() ||
        !d3d11_glyph_text_rebuild_title(&owner,renderer.graphics.context,renderer.dpi,false)) return false;
    auto* session = owner.system.native_fonts;
    const NativeFontRun* base = nullptr;
    for (u32 i = 0; i < owner.font_face_count; ++i) {
        const auto& entry = owner.font_faces[i];
        if (!(entry.flags & 2u)) {
            base = native_font_run_get(*native_font_session_runs(session), entry.native_face);
            if (base && !base->simulations) break;
            base = nullptr;
        }
    }
    if (!base) return false;
    const auto instance = base->instance; const auto selection = base->source;
    NativeFontRunHandle handle{};
    struct ReleaseRun {
        NativeFontSession* session; NativeFontRunHandle& run;
        ~ReleaseRun() noexcept { if (run.slot) native_font_run_release(native_font_session_runs(session), run); }
    } release_run{session, handle};
    GlyphQuad quad{}; u32 count = 0; bool invalidated = false;
    TextRun run{}; run.native_font = true; run.em_size = 16;
    ShapedText text{}; text.runs = &run; text.run_count = 1; text.set_native_fonts(session);
    D3D11GlyphTextAppendRequest request{}; request.text = &text; request.output = &quad;
    request.count = &count; request.capacity = 1; request.atlas_invalidated = &invalidated;
    request.dpi = renderer.dpi; request.color = {1,1,1,1};
    const u32 initial_faces = owner.font_face_count;
    for (u32 mode = 0; mode < 4; ++mode) {
        if (handle.slot) native_font_run_release(native_font_session_runs(session), handle);
        handle = {};
        if (!native_font_session_run(session, instance, selection, mode, &handle).ok()) return false;
        run.native_face = handle;
        for (u32 pass = 0; pass < 2; ++pass)
            if (!d3d11_glyph_text_append(&owner, renderer.graphics.context, request) ||
                owner.font_face_count != initial_faces + mode) return false;
    }
    text.set_native_fonts(renderer.glyph_text.system.native_fonts);
    if (d3d11_glyph_text_append(&owner, renderer.graphics.context, request)) return false;
    if (count || invalidated || owner.font_face_count != initial_faces + 3) return false;
    log_write(LogLevel::info, "probe", "renderer_font_simulations_and_foreign_session_passed");
    return true;
}
}
