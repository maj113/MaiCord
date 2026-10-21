// Pure key classification, included in the implementation namespace.
bool supported_key(const MediaAssetKey& key) noexcept {
    if (!media_asset_key_valid(key)) {
        return false;
    }
    switch (key.kind) {
    case MediaAssetKind::regional_flag: {
        char target[kRegionalFlagSvgTargetBytes];
        return regional_flag_svg_target(static_cast<u16>(key.id), {target, sizeof(target)}) != 0;
    }
    case MediaAssetKind::catalog_preview:
    case MediaAssetKind::guild_icon:
    case MediaAssetKind::sticker:
    case MediaAssetKind::custom_emoji:
    case MediaAssetKind::user_avatar:
    case MediaAssetKind::channel_icon:
    case MediaAssetKind::attachment:
    case MediaAssetKind::embed:
    case MediaAssetKind::profile_image:
        return true;
    default:
        return false;
    }
}

bool webp_asset(const MediaAssetKey& key) noexcept {
    return key.kind == MediaAssetKind::custom_emoji || key.kind == MediaAssetKind::guild_icon ||
           key.kind == MediaAssetKind::user_avatar || key.kind == MediaAssetKind::channel_icon ||
           (key.kind == MediaAssetKind::catalog_preview &&
            media_asset_catalog_format(key) == MediaAttachmentFormat::webp) ||
           (key.kind == MediaAssetKind::attachment &&
            media_asset_attachment_format(key) == MediaAttachmentFormat::webp) ||
           ((key.kind == MediaAssetKind::embed || key.kind == MediaAssetKind::profile_image) &&
            media_asset_embed_format(key) == MediaAttachmentFormat::webp);
}
bool lottie_asset(const MediaAssetKey& key) noexcept {
    return (key.kind == MediaAssetKind::sticker && key.variant == 3) ||
           (key.kind == MediaAssetKind::catalog_preview &&
            media_asset_catalog_format(key) == MediaAttachmentFormat::lottie);
}

bool animated_asset(const MediaAssetKey& key) noexcept {
    return (key.flags & media_asset_flag_animated) != 0;
}

bool apng_asset(const MediaAssetKey& key) noexcept {
    return (key.kind == MediaAssetKind::sticker && key.variant == 2) ||
           (key.kind == MediaAssetKind::catalog_preview &&
            media_asset_catalog_format(key) == MediaAttachmentFormat::png && animated_asset(key)) ||
           (key.kind == MediaAssetKind::profile_image &&
            media_asset_embed_format(key) == MediaAttachmentFormat::png && animated_asset(key));
}

bool gif_asset(const MediaAssetKey& key) noexcept {
    return (key.kind == MediaAssetKind::sticker && key.variant == 4) ||
           (key.kind == MediaAssetKind::catalog_preview &&
            media_asset_catalog_format(key) == MediaAttachmentFormat::gif) ||
           (key.kind == MediaAssetKind::attachment &&
            media_asset_attachment_format(key) == MediaAttachmentFormat::gif) ||
           ((key.kind == MediaAssetKind::embed || key.kind == MediaAssetKind::profile_image) &&
            media_asset_embed_format(key) == MediaAttachmentFormat::gif);
}

bool video_asset(const MediaAssetKey& key) noexcept {
    if (key.kind == MediaAssetKind::attachment)
        return media_asset_attachment_format(key) == MediaAttachmentFormat::mp4;
    // A VIDEO GIF favourite's picker tile: a muted looping animation.
    if (key.kind == MediaAssetKind::catalog_preview)
        return media_asset_catalog_format(key) == MediaAttachmentFormat::mp4;
    return key.kind == MediaAssetKind::embed && animated_asset(key) &&
           media_asset_embed_resource(key) == MediaEmbedResource::video &&
           media_asset_embed_format(key) == MediaAttachmentFormat::mp4;
}

bool catalog_png_key(const MediaAssetKey& key) noexcept {
    return key.kind == MediaAssetKind::catalog_preview &&
           media_asset_catalog_format(key) == MediaAttachmentFormat::png;
}

