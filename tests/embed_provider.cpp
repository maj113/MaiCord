#include "maicord/protocol/embed_provider.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool value, const char* label) noexcept { test_check(&failures, value, label); }
void youtube(Utf8View url, EmbedProvider provider, u32 start = 0) noexcept {
    const auto result = embed_video_locator(url);
    check(result.provider == provider && utf8_equal(result.id, utf8_literal("aB_01234-xy")) &&
          result.start_seconds == start, "YouTube locator and start position");
}
} // namespace
}
extern "C" __declspec(noreturn) void WINAPI maicord_embed_provider_entry() noexcept {
    using namespace maicord;
    using P = EmbedProvider;
    youtube(utf8_literal("https://www.youtube.com/watch?v=aB_01234-xy&t=1h2m3s"), P::youtube, 3723);
    youtube(utf8_literal("https://music.youtube.com/watch?v=aB_01234-xy&list=RDtest"), P::youtube_music);
    youtube(utf8_literal("https://youtu.be/aB_01234-xy?si=opaque&start=42"), P::youtube, 42);
    youtube(utf8_literal("https://WWW.YouTube.com/shorts/aB_01234-xy"), P::youtube);
    youtube(utf8_literal("https://www.youtube-nocookie.com/embed/aB_01234-xy?start=90"), P::youtube, 90);
    youtube(utf8_literal("https://youtube.com/live/aB_01234-xy?t=90000"), P::youtube, 90000);
    constexpr Utf8View bad[]{
        utf8_literal("http://youtube.com/watch?v=aB_01234-xy"),
        utf8_literal("https://youtube.com.evil.test/watch?v=aB_01234-xy"),
        utf8_literal("https://youtube.com@evil.test/watch?v=aB_01234-xy"),
        utf8_literal("https://evil.test@youtube.com/watch?v=aB_01234-xy"),
        utf8_literal("https://youtube.com:443/watch?v=aB_01234-xy"),
        utf8_literal("https://%79outube.com/watch?v=aB_01234-xy"),
        utf8_literal("https://youtube.com\\evil.test/watch?v=aB_01234-xy"),
        utf8_literal("https://youtube.com/watch?v=short"),
        utf8_literal("https://youtube.com/watch?v=aB_01234-xy&v=aB_01234-xy"),
        utf8_literal("https://youtube.com/embed/aB_01234-xy/extra"),
        utf8_literal("https://youtube.com/watch?v=aB_01234-xy&t=4294967296"),
        utf8_literal("https://youtube.com/watch?v=aB_01234-xy&t=1m2h"),
        utf8_literal("https://youtube.com/watch?v=aB_01234-xy&t=1s&t=2s"),
        utf8_literal("https://youtube.com/playlist?list=PLtest"),
        utf8_literal("https://youtube.com/watch?v=aB_01234-xy\r\nHeader: injected")};
    for (auto url : bad) check(!embed_video_locator(url).id.bytes, "reject ambiguous/invalid video locator");
    check(embed_provider({nullptr, 10}) == P::none, "null input");
    check(embed_provider(utf8_literal("https://open.spotify.com/track/123")) == P::spotify, "Spotify host");
    check(embed_provider(utf8_literal("https://music.apple.com/us/album/name/123?i=456")) == P::apple_music,
          "Apple Music host");
    check(embed_provider(utf8_literal("https://music.amazon.co.uk/albums/123")) == P::amazon_music,
          "regional Amazon Music host");
    check(embed_provider(utf8_literal("https://music.amazon.com.evil.test/albums/123")) == P::none,
          "Amazon suffix boundary");
    check(embed_provider(utf8_literal("https://player.twitch.tv/?channel=test")) == P::twitch, "Twitch host");
    check(embed_provider(utf8_literal("https://www.tiktok.com/player/v1/123")) == P::tiktok, "TikTok host");
    check(embed_provider(utf8_literal("https://session-share.playstation.com/123")) == P::playstation,
          "PlayStation host");
    ExitProcess(failures ? 1 : 0);
}
