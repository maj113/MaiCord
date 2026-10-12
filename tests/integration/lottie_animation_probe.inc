u64 lottie_atlas_hash(D3D11Renderer& renderer,MediaAssetKey key) noexcept {
    const auto a=media_atlas_find(renderer.preview_media_atlas,key);if(!a.ok())return 0;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=128;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging=nullptr;
    if(FAILED(renderer.graphics.device->CreateTexture2D(&desc,nullptr,&staging)))return 0;
    const D3D11_BOX box{a.x,a.y,0,a.x+128u,a.y+128u,1};auto* context=renderer.graphics.context;
    context->CopySubresourceRegion(staging,0,0,0,0,renderer.media_gpu.preview_texture,0,&box);
    D3D11_MAPPED_SUBRESOURCE mapped{};u64 hash=0;
    if(SUCCEEDED(context->Map(staging,0,D3D11_MAP_READ,0,&mapped))){hash=14695981039346656037ull;
        for(u32 y=0;y<128;++y)for(u32 x=0;x<512;++x){hash^=static_cast<const u8*>(mapped.pData)[y*mapped.RowPitch+x];hash*=1099511628211ull;}
        context->Unmap(staging,0);}
    staging->Release();return hash;
}
bool probe_lottie_worker_animation(MediaAssetKey key,ConstSlice<u8> json,UiMediaPickerScene& gallery) noexcept {
    auto* worker=static_cast<AccountMediaWorker*>(VirtualAlloc(nullptr,sizeof(AccountMediaWorker),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    auto* app=static_cast<app_detail::Application*>(VirtualAlloc(nullptr,sizeof(app_detail::Application),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!worker || !app)return false;
    bool initialized=account_media_worker_initialize(worker).ok();
    bool okay=initialized && animation_visual_runtime_initialize(&app->animations);
    app->renderer=g_probe.renderer;app->renderer.media_picker_scene=&gallery;
    app->clock=clock_initialize();app->process_start=clock_now();
    app_detail::media_picker_probe_set_key(key);
    app_detail::ApplicationMediaPump pump(*app);
    const auto store=okay?encoded_media_cache_store(&worker->lanes[0].media.cache,key,json,
        {{},~0ull,true,false}):EncodedMediaCacheStoreResult{};
    AccountMediaCatalogPreviewRequest request{utf8_literal("https://cdn.discordapp.com/stickers/42.json"),key,99,1,128,128,MediaAttachmentFormat::lottie};
    if(okay)okay=store.stored() && account_media_worker_request_catalog_preview(worker,request,
        clock_monotonic_milliseconds(),clock_unix_milliseconds()/1000).state==AccountMediaRequestState::queued;
    AccountMediaFrame first{};
    if(okay){WaitForSingleObject(worker->output_event,3000);
        okay=account_media_worker_take_frame(worker,&first) && first.kind==AccountMediaFrameKind::lottie_paint &&
            first.content_identity && first.frame_count>10 && first.duration_us && first.cache_generation;
        if(okay){log_u64(LogLevel::info,"probe","lottie_current",pump.media_key_is_current(key));
            const auto uploaded=pump.upload_account_media_frame(first);
            log_u64(LogLevel::info,"probe","lottie_upload_status",static_cast<u64>(uploaded));
            okay=uploaded==app_detail::ApplicationMediaPump::MediaFrameUploadResult::uploaded;}}
    log_u64(LogLevel::info,"probe","lottie_content_identity",first.content_identity);
    log_u64(LogLevel::info,"probe","lottie_cache_generation",first.cache_generation);
    log_u64(LogLevel::info,"probe","lottie_initial_upload_ok",okay);
    log_u64(LogLevel::info,"probe","lottie_first_kind",static_cast<u64>(first.kind));
    log_u64(LogLevel::info,"probe","lottie_first_frame_count",first.frame_count);
    const auto timeline=animation_timeline_find(&app->animations.timeline,key);
    okay=okay && timeline.ok() && media_atlas_decoded_animation(*app->renderer.preview_media_atlas,
        media_atlas_find(app->renderer.preview_media_atlas,key).handle);
    log_u64(LogLevel::info,"probe","lottie_timeline_found",timeline.ok());
    const u64 first_hash=okay?lottie_atlas_hash(app->renderer,key):0;
    for(u32 i=1;okay && i<10;++i){
        auto& entry=app->animations.timeline.entries[timeline.slot];
        animation_timeline_begin_visibility(&app->animations.timeline);
        animation_timeline_observe_visible(&app->animations.timeline,key,entry.due_at_us);
        animation_timeline_end_visibility(&app->animations.timeline,entry.due_at_us);
        AnimationFrameAdvance advance{};
        const auto collected=animation_timeline_collect_due(&app->animations.timeline,entry.due_at_us,{&advance,1});
        log_u64(LogLevel::info,"probe","lottie_advance_collected",collected.advances);
        okay=collected.ok() && collected.advances==1 && advance.frame_index==i &&
            account_media_worker_advance_animation(worker,key,first.cache_generation,i,
                clock_unix_milliseconds()/1000).state==AccountMediaRequestState::queued;
        if(!okay)break;
        WaitForSingleObject(worker->output_event,3000);AccountMediaFrame frame{};bool repaint=false;
        okay=account_media_worker_take_frame(worker,&frame) && frame.kind==AccountMediaFrameKind::lottie_paint &&
            frame.frame_index==i && frame.content_identity==first.content_identity &&
            pump.apply_animation_frame(advance,frame,&repaint) && repaint;
    }
    log_u64(LogLevel::info,"probe","lottie_all_advances_ok",okay);
    const u64 last_hash=okay?lottie_atlas_hash(app->renderer,key):0;
    okay=okay && first_hash && last_hash && first_hash!=last_hash;
    log_u64(LogLevel::info,"probe","lottie_pixels_changed",first_hash!=last_hash);
    AnimationFrameCacheResult cached{};
    if(okay)okay=renderer_restore_animation_frame(&app->renderer,
        {key,0,first.cache_generation,0,first.content_identity},&cached) && cached.kind==AnimationFrameCacheKind::lottie &&
        lottie_atlas_hash(app->renderer,key)==first_hash;
    if(initialized)okay=account_media_worker_stop(worker,5000) && okay;
    animation_visual_runtime_release(&app->animations);
    // This borrowed-renderer probe cannot move the cache's inline row storage.
    // Replay has already materialized its pixels in the live preview atlas.
    animation_frame_cache_gpu_release(&app->renderer.preview_animation_gpu);
    g_probe.renderer=app->renderer;app_detail::media_picker_probe_set_key({});
    VirtualFree(app,0,MEM_RELEASE);VirtualFree(worker,0,MEM_RELEASE);
    log_write(okay?LogLevel::info:LogLevel::error,"probe",okay?"lottie_worker_timeline_advances_and_gpu_cache_replay_passed":"lottie_worker_animation_failed");
    return okay;
}
