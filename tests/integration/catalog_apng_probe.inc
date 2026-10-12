bool probe_catalog_apng() noexcept {
    ApngFixtureResult fixture{};ApngWorkspace workspace{};ApngImage image{};
    DeflateWorkspace deflate{};
    if(!build_rgba8_apng_fixture({g_probe.png_encoded,sizeof(g_probe.png_encoded)},
        {g_probe.png_zlib,sizeof(g_probe.png_zlib)},&fixture) ||
        !apng_parse({g_probe.png_encoded,fixture.encoded_bytes},&image,&workspace).ok())return false;
    const auto key=media_asset_catalog_preview(0xaffe01,1,MediaAttachmentFormat::png,true);
    ApngFrame prior{};
    for(u32 i=0;i<image.frame_count;++i){PngImage frame{};
        const auto decoded=apng_decode_parsed_frame({g_probe.png_encoded,fixture.encoded_bytes},image,i,
            &deflate,&frame,{g_probe.png_scanlines,sizeof(g_probe.png_scanlines)});
        if(!decoded.ok())return false;
        const auto uploaded=i?renderer_advance_apng(&g_probe.renderer,key,prior,decoded.frame,frame,
            {g_probe.png_scanlines,frame.inflated_bytes}):renderer_begin_apng(&g_probe.renderer,key,
            static_cast<u16>(image.canvas.width),static_cast<u16>(image.canvas.height),decoded.frame,frame,
            {g_probe.png_scanlines,frame.inflated_bytes});
        if(!uploaded.ok() || !media_atlas_find(g_probe.renderer.preview_media_atlas,key).ok() ||
            media_atlas_find(g_probe.renderer.media_atlas,key).ok() ||
            !apng_atlas_pixels_exact(uploaded.allocation,i,true))return false;
        AnimationFrameCacheDescriptor descriptor{};descriptor.key={key,i,1,0,0x12345};
        descriptor.kind=AnimationFrameCacheKind::apng;descriptor.control.apng=decoded.frame;
        descriptor.width=descriptor.height=4;descriptor.duration_us=50000;
        if(!renderer_capture_animation_frame(&g_probe.renderer,descriptor))return false;
        prior=decoded.frame;
    }
    AnimationFrameCacheResult restored{};
    if(!renderer_restore_animation_frame(&g_probe.renderer,{key,0,1,0,0x12345},&restored) ||
        !apng_atlas_pixels_exact(media_atlas_find(g_probe.renderer.preview_media_atlas,key),0,true))return false;
    log_write(LogLevel::info,"probe","catalog_apng_preview_atlas_pixels_and_cached_replay_passed");
    return true;
}
