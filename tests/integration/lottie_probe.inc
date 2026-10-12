// Offline fixture probe: typed JSON reaches the native gallery and GPU atlas.
bool probe_lottie_gallery() noexcept {
    wchar_t path[4096]{};
    const DWORD chars = GetEnvironmentVariableW(L"MAICORD_LOTTIE_FIXTURE", path, 4096);
    HANDLE file = chars && chars < 4096 ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)
                                        : INVALID_HANDLE_VALUE;
    LARGE_INTEGER size{};
    const bool bounded = file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
                         size.QuadPart <= 1024 * 1024;
    auto* json = bounded ? static_cast<u8*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(size.QuadPart),
                                                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                         : nullptr;
    DWORD bytes = 0;
    const bool loaded = json && ReadFile(file, json, static_cast<DWORD>(size.QuadPart), &bytes, nullptr) &&
                        bytes == size.QuadPart;
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    UiMediaPickerScene gallery{};
    // The account module's media worker compiles stickers; the renderer only
    // rasterizes the borrowed program.
    auto* program = static_cast<ColrPaintProgram*>(
        VirtualAlloc(nullptr, sizeof(ColrPaintProgram), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    struct Release {
        u8* json;
        ColrPaintProgram* program;
        UiMediaPickerScene& gallery;
        ~Release() noexcept {
            g_probe.renderer.media_picker_scene = nullptr;
            ui_media_picker_scene_release(&gallery);
            if (json)
                VirtualFree(json, 0, MEM_RELEASE);
            if (program) {
                arena_release(&program->storage);
                VirtualFree(program, 0, MEM_RELEASE);
            }
        }
    } release{json, program, gallery};
    const auto key = media_asset_catalog_preview(0xaffe00, 1, MediaAttachmentFormat::lottie, true);
    if (!loaded || !program || !probe_present(&g_probe.renderer, kDefaultTheme))
        return false;
    LottieTiming timing{};
    if (!lottie_program_compile_frame({json, bytes}, 0, 128, &timing,
                                      +[](void* context) noexcept { return static_cast<ColrPaintProgram*>(context); },
                                      program)) {
        log_write(LogLevel::error, "probe", "lottie_compile_failed");
        return false;
    }
    const auto upload = renderer_upload_lottie_program(&g_probe.renderer, key, *program);
    if (!upload.ok()) {
        log_u64(LogLevel::error, "probe", "lottie_upload_error", static_cast<u64>(upload.error));
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 128;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_probe.renderer.graphics.device->CreateTexture2D(&desc, nullptr, &staging)))
        return false;
    const auto& a = upload.allocation;
    const D3D11_BOX box{a.x, a.y, 0, a.x + 128u, a.y + 128u, 1};
    auto* context = g_probe.renderer.graphics.context;
    context->CopySubresourceRegion(staging, 0, 0, 0, 0, g_probe.renderer.media_gpu.preview_texture, 0,
                                   &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        staging->Release();
        return false;
    }
    u32 ink = 0;
    wchar_t poster_path[4096]{};
    if(GetEnvironmentVariableW(L"MAICORD_LOTTIE_POSTER_DUMP",poster_path,4096)) {
        HANDLE poster=CreateFileW(poster_path,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(poster==INVALID_HANDLE_VALUE){context->Unmap(staging,0);staging->Release();return false;}
        bool saved=true;DWORD written=0;
        for(u32 y=0;y<128&&saved;++y)
            saved=WriteFile(poster,static_cast<const u8*>(mapped.pData)+y*mapped.RowPitch,128*4,&written,nullptr)&&written==128*4;
        CloseHandle(poster);
        if(!saved){context->Unmap(staging,0);staging->Release();return false;}
    }
    for (u32 y = 0; y < 128; ++y)
        for (u32 x = 0; x < 128; ++x)
            ink += static_cast<const u8*>(mapped.pData)[y * mapped.RowPitch + x * 4 + 3] > 32;
    context->Unmap(staging, 0);
    staging->Release();
    if (ink < 32) {
        log_u64(LogLevel::error, "probe", "lottie_empty_poster_pixels", ink);
        return false;
    }
    struct Fixture {
        MediaAssetKey key;
        bool disabled;
    } fixture{key, GetEnvironmentVariableW(L"MAICORD_LOTTIE_DISABLED_PROBE", nullptr, 0) != 0};
    UiMediaPickerSection sections[14]{};
    for(u32 i=0;i<14;++i) sections[i]={i+1,30*i,30,utf8_literal("Mallow The Rascal"),true};
    UiMediaPickerSource source{
        &fixture, 1, 1, 420, 1, {sections,14}, +[](void* p, u32 i, UiMediaPickerItem* out) noexcept {
            const auto key = static_cast<Fixture*>(p)->key;
            *out = {};
            out->id = key.id + i;
            out->asset_id = key.id;
            out->variant = key.variant;
            out->asset_flags = key.flags;
            out->kind = UiMediaPickerItemKind::sticker;
            out->enabled = !static_cast<Fixture*>(p)->disabled;
            out->label = utf8_literal("Native sticker");
            out->width = out->height = 128;
            return true;
        }};
    UiMediaPickerContent content{};
    content.account_scope = source.account_scope;
    content.query_generation = source.query_generation;
    content.revision = 1;
    content.kind = UiMediaPickerKind::sticker;
    content.phase = UiMediaPickerPhase::ready;
    content.available_tabs = 7;
    content.edit.active = true;
    content.placeholder = utf8_literal("Search stickers");
    const auto* anchor =
        message_composer_scene_layout_node(g_probe.message_composer_scene, MessageComposerControl::gif);
    if (!anchor) {
        log_write(LogLevel::error, "probe", "lottie_anchor_missing");
        return false;
    }
    if (!ui_media_picker_scene_update(
            &gallery, &source, &content,
            ui_scene_content_rectangle(*g_probe.renderer.ui_scene, UiSemantic::content),
            anchor->rectangle)) {
        log_write(LogLevel::error, "probe", "lottie_scene_update_failed");
        return false;
    }
    g_probe.renderer.media_picker_scene = &gallery;
    g_probe.renderer.layer_opacity.popup = 1;
    if(GetEnvironmentVariableW(L"MAICORD_LOTTIE_ANIMATION_PROBE",nullptr,0) &&
        !probe_lottie_worker_animation(key,{json,bytes},gallery))return false;
    if(GetEnvironmentVariableW(L"MAICORD_LOTTIE_REJECTION_PROBE",nullptr,0)){
        auto* application=static_cast<app_detail::Application*>(VirtualAlloc(nullptr,
            sizeof(app_detail::Application),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        if(!application)return false;
        // Borrow the initialized renderer only for a rejected upload. No COM
        // ownership or renderer shutdown is transferred to this temporary host.
        application->renderer=g_probe.renderer;
        app_detail::media_picker_probe_set_key(key);
        app_detail::ApplicationMediaPump pump(*application);
        // Paint bytes that are not one worker-compiled program are rejected.
        constexpr auto bad=utf8_literal(R"({"w":320,"h":320,"layers":[{"ty":99}]})");
        AccountMediaFrame frame{};frame.key=key;frame.width=frame.height=128;frame.row_pitch=512;
        frame.kind=AccountMediaFrameKind::lottie_paint;frame.source_catalog_scope=99;
        frame.source_catalog_generation=1;frame.bytes={reinterpret_cast<const u8*>(bad.data),bad.bytes};
        const auto result=pump.upload_account_media_frame(frame);
        const bool rejected=result==app_detail::ApplicationMediaPump::MediaFrameUploadResult::deferred &&
            app_detail::media_picker_probe_rejections()==1 && gallery.active;
        app_detail::media_picker_probe_set_key({});VirtualFree(application,0,MEM_RELEASE);
        if(!rejected)return false;
        log_write(LogLevel::info,"probe","catalog_upload_rejection_does_not_stop_account_passed");
    }
    if (!renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
        !probe_present(&g_probe.renderer, kDefaultTheme) || !probe_capture_frame(L"lottie-gallery.bmp"))
        return false;
    u32 random = 0x38473;
    for (u32 i = 0; i < 1000; ++i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        const float max = ui_media_picker_scene_scrollbar(gallery).maximum_scroll;
        ui_media_picker_scene_scroll(&gallery, max * static_cast<float>(random & 65535) / 65535 -
                                                   gallery.scroll_dips);
        if (!gallery.active || !renderer_rebuild_content_text(&g_probe.renderer, kDefaultTheme) ||
            (fixture.disabled && !probe_present(&g_probe.renderer,kDefaultTheme)))
            return false;
    }
    return true;
}
