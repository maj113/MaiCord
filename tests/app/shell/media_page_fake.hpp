#pragma once
// A stand-in for the media page store (src/app/media/media_page.cpp) and the message viewer's page source, for the
// tests of the tab model: pages are slots that record what the tabs asked of them. One test source includes it.
#include "../../../src/app/media/image_viewer.hpp"
#include "../../../src/app/media/media_page.hpp"
#include "../../../src/app/media/video.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::media_page_fake {

struct Page {
    bool open, missing;
    app_detail::MediaPageKind kind;
    u8 title_bytes;
    char title[40];
};
inline Page pages[app_detail::kMediaPageCapacity + 1];
// The page that shows, 0 for none.
inline u8 shown;
// The message viewer's current item, while `viewer_open`; showing a page closes the viewer.
inline bool viewer_open;
inline app_detail::MediaPageSource viewer_source;
// How often the pages were all closed.
inline u32 resets;
// Each page's player (the tab's icon) and how often a tab icon toggled it.
inline app_detail::VideoPagePlayback playback[app_detail::kMediaPageCapacity + 1];
inline u32 toggles;

inline u32 open_pages() noexcept {
    u32 count = 0;
    for (const Page& page : pages)
        count += page.open;
    return count;
}

inline void reset() noexcept {
    clear_bytes(pages, sizeof(pages));
    shown = 0;
    viewer_open = false;
    viewer_source = {};
    resets = 0;
    clear_bytes(playback, sizeof(playback));
    toggles = 0;
}

inline app_detail::MediaPageSource source(app_detail::MediaPageKind kind, const char* title) noexcept {
    app_detail::MediaPageSource result{};
    result.kind = kind;
    u32 bytes = 0;
    while (title[bytes])
        ++bytes;
    result.title = {title, bytes};
    return result;
}
} // namespace maicord::media_page_fake

namespace maicord::app_detail {
u8 application_media_page_open(const MediaPageSource& source) noexcept {
    namespace fake = media_page_fake;
    for (u8 id = 1; id <= kMediaPageCapacity; ++id) {
        fake::Page& page = fake::pages[id];
        if (page.open)
            continue;
        page = {true, false, source.kind, 0, {}};
        page.title_bytes = static_cast<u8>(source.title.bytes < sizeof(page.title) ? source.title.bytes : sizeof(page.title));
        copy_bytes(page.title, source.title.data, page.title_bytes);
        return id;
    }
    return 0;
}
bool application_media_page_show(u8 id) noexcept {
    if (id > kMediaPageCapacity || !media_page_fake::pages[id].open)
        return false;
    media_page_fake::viewer_open = false;
    media_page_fake::shown = id;
    return true;
}
void application_media_page_hide() noexcept {
    media_page_fake::shown = 0;
}
void application_media_page_close(u8 id) noexcept {
    if (media_page_fake::shown == id)
        media_page_fake::shown = 0;
    media_page_fake::pages[id] = {};
}
void application_media_page_reset() noexcept {
    clear_bytes(media_page_fake::pages, sizeof(media_page_fake::pages));
    media_page_fake::shown = 0;
    ++media_page_fake::resets;
}
u8 application_media_page_shown() noexcept {
    return media_page_fake::shown;
}
MediaPageKind application_media_page_kind(u8 id) noexcept {
    return media_page_fake::pages[id].kind;
}
Utf8View application_media_page_title(u8 id) noexcept {
    return {media_page_fake::pages[id].title, media_page_fake::pages[id].title_bytes};
}
bool application_media_page_missing(u8 id) noexcept {
    return media_page_fake::pages[id].missing;
}
bool application_image_viewer_page_source(MediaPageSource* source) noexcept {
    if (!media_page_fake::viewer_open)
        return false;
    if (source)
        *source = media_page_fake::viewer_source;
    return true;
}
VideoPagePlayback application_video_page_playback(u8 id) noexcept {
    return media_page_fake::playback[id];
}
bool application_video_page_toggle(u8 id) noexcept {
    ++media_page_fake::toggles;
    auto& playback = media_page_fake::playback[id];
    playback = playback == VideoPagePlayback::playing ? VideoPagePlayback::paused : VideoPagePlayback::playing;
    return true;
}
} // namespace maicord::app_detail
