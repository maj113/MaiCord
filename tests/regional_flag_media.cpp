#include "maicord/app/account_media.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/render/colr_program.hpp"
#include "maicord/media/regional_flag_svg.hpp"

namespace maicord {
void test_regional_flag_media(void (*check)(bool,const char*) noexcept) noexcept {
    AccountMedia media{};
    const auto initialized=account_media_initialize(&media);
    check(initialized.ok()&&media.started,"regional flags: existing account media owner initializes");
    if(!initialized.ok()||!media.started)return;
    const auto no_codec_owners=[&]() noexcept {
        return !media.webp_workspace&&!media.lossy_webp_workspace&&!media.animated_webp_workspace&&
            !media.apng_workspace&&!media.gif_workspace&&!media.jpeg_workspace&&!media.mp4_decoder&&
            !media.expanded_scanlines&&!media.client_started;
    };
    const auto no_leases=[&]() noexcept {
        for(u32 i=0;i<media.pipeline.cache->entry_capacity;++i)if(media.pipeline.cache->entries[i].leases)return false;
        return true;
    };
    const auto allocation=media.allocation;
    const auto scratch=media.pipeline.scanline_storage;
    const auto capacity=media.pipeline.scanline_capacity;
    const auto key=media_asset_regional_flag(504);
    // A synthetic cached body for the same flag: the bundle stays authoritative.
    constexpr auto svg=utf8_literal("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 36 36\"><path d=\"M0 0T1 1\"/></svg>");
    const ConstSlice<u8> bytes{reinterpret_cast<const u8*>(svg.data),svg.bytes};
    check(encoded_media_cache_store(media.pipeline.cache,key,bytes,{{},1000,true,false}).stored(),
          "regional flags: encoded cache admits an unrelated flag body");
    for(u32 i=0;i<2;++i){
        const auto requested=account_media_request_static_asset(&media,key,100+i,10);
        check(requested.ok()&&requested.state==AccountMediaRequestState::ready,
              "regional flags: request publishes the bundled flag without network");
        AccountMediaFrame frame{};const bool taken=account_media_take_frame(&media,&frame);
        const auto* program=reinterpret_cast<const ColrPaintProgram*>(frame.bytes.data);
        check(taken&&frame.kind==AccountMediaFrameKind::regional_flag_paint&&media_asset_key_equal(frame.key,key)&&
            frame.width==64&&frame.height==64&&frame.row_pitch==256&&frame.bytes.data==media.lottie_workspace&&
            frame.bytes.count==sizeof(ColrPaintProgram)&&program->storage.base&&program->storage.offset&&
            !frame.duration_us&&!frame.frame_count&&!frame.image&&!frame.gpu_video,
            "regional flags: frame kind 11 borrows the worker-compiled paint program");
        check(no_codec_owners()&&media.allocation==allocation&&media.pipeline.scanline_storage==scratch&&
            media.pipeline.scanline_capacity==capacity&&no_leases()&&
            encoded_media_cache_find(media.pipeline.cache,key,10).found(),
            "regional flags: repeat publication retains owners and leaves the cache untouched");
    }
    const auto unsupported=account_media_request_static_asset(&media,media_asset_regional_flag(1),202,10);
    check(unsupported.state==AccountMediaRequestState::unsupported&&!media.client_started,
        "regional flags: unlisted RI pair cannot authorize a network request");
    constexpr auto json=utf8_literal(R"({"w":320,"h":320,"layers":[],"fr":60,"ip":0,"op":120})");
    const auto vector=media_asset_catalog_preview(42,7,MediaAttachmentFormat::lottie,false);
    check(encoded_media_cache_store(media.pipeline.cache,vector,
        {reinterpret_cast<const u8*>(json.data),json.bytes},{{},1000,true,false}).stored(),
        "Lottie: exact generation owns validated encoded artwork in the existing cache");
    const AccountMediaCatalogPreviewRequest preview{utf8_literal("https://cdn.discordapp.com/stickers/42.json"),
        vector,99,7,128,128,MediaAttachmentFormat::lottie};
    const auto requested=account_media_request_catalog_preview(&media,preview,300,10);
    AccountMediaFrame frame{};
    check(requested.state==AccountMediaRequestState::ready && account_media_take_frame(&media,&frame) &&
        frame.kind==AccountMediaFrameKind::lottie_paint && frame.source_kind==AccountMediaSourceKind::catalog_preview &&
        frame.source_catalog_scope==99 && frame.source_catalog_generation==7 && frame.width==128 && frame.row_pitch==512 &&
        frame.bytes.count==sizeof(ColrPaintProgram) && frame.cache_generation && frame.frame_count==120 &&
        frame.duration_us==16667 && media.lottie_workspace &&
        no_codec_owners() && no_leases(),
        "Lottie: worker paint frame retains timing, cache identity and exact catalog owner without a raster codec");
    const auto generation=frame.cache_generation;
    check(account_media_advance_animation(&media,vector,generation,15,10).state==AccountMediaRequestState::ready &&
        account_media_take_frame(&media,&frame) && frame.kind==AccountMediaFrameKind::lottie_paint && frame.frame_index==15 &&
        frame.frame_count==120 && frame.cache_generation==generation && no_leases(),
        "Lottie: a later frame compiles from the leased immutable source without refetching");
    check(account_media_stop(&media,1000)&&!media.started&&!media.allocation&&!media.lottie_workspace,
          "regional flags: existing stop releases the media owner without new workers or codec banks");
    check(account_media_initialize(&media).ok(),"local flags: fresh media owner initializes");
    u32 local_count=0;bool local_ok=media.started;
    static u8 bundle_scratch[kRegionalFlagSvgMaximumBytes];
    const Slice<u8> flag_scratch{bundle_scratch,sizeof(bundle_scratch)};
    u64 checksum=0xCBF29CE484222325ull;
    for(u16 sequence=1;local_ok && sequence<680;++sequence){
        const auto local=regional_flag_bundled_svg(sequence,flag_scratch);
        if(!local.count)continue;
        ++local_count;
        for(u32 i=0;i<local.count;++i)checksum=(checksum^local.data[i])*0x100000001B3ull;
        const auto flag=media_asset_regional_flag(sequence);
        const auto request=account_media_request_static_asset(&media,flag,30000+sequence,10);
        AccountMediaFrame local_frame{};
        local_ok=request.state==AccountMediaRequestState::ready && account_media_take_frame(&media,&local_frame) &&
            local_frame.kind==AccountMediaFrameKind::regional_flag_paint && local_frame.width==64 && local_frame.height==64 &&
            local_frame.bytes.count==sizeof(ColrPaintProgram) && local_frame.bytes.data==media.lottie_workspace &&
            reinterpret_cast<const ColrPaintProgram*>(local_frame.bytes.data)->storage.offset &&
            !media.client_started && no_codec_owners() && !media.pipeline.cache->entry_count;
    }
    check(checksum==regional_flag_bundled_text_checksum(),
        "local flags: rebuilt SVG text matches the generator's verified text byte for byte");
    check(local_ok && local_count==262 && !regional_flag_bundled_svg(1,flag_scratch).count &&
        !regional_flag_bundled_svg(680,flag_scratch).count &&
        !regional_flag_bundled_svg(3,{bundle_scratch,16}).count,
        "local flags: all 262 pinned SVGs validate and publish on a cold owner without network or cache entries");
    // Groups inflate on every call: descending and alternating access must
    // re-inflate and still return byte-exact, parseable artwork.
    bool groups_ok=true;u32 groups_seen=0;
    for(u16 step=0;groups_ok && step<679;++step){
        const u16 sequence=static_cast<u16>(step&1 ? 679-step/2 : 1+step/2);
        const auto flag=regional_flag_bundled_svg(sequence,flag_scratch);
        if(!flag.count)continue;
        ++groups_seen;
        RegionalFlagSvg image{};
        groups_ok=flag.count>6 && flag.data[0]==u8('<') && flag.data[flag.count-1]==u8('>') &&
            regional_flag_svg_parse(flag,&image) && image.count;
    }
    check(groups_ok && groups_seen==262,"local flags: alternating group access re-inflates exact parseable SVGs");
    check(account_media_stop(&media,1000),"local flags: media owner stops");
}
} // namespace maicord
