// Signed channel changelog: the publisher's exact layout (byte-for-byte the
// document tools/release/test_release_tools.py renders), its signature
// domain, schema and bounds, untrusted-text cleanup, the display subset and
// date text. Synthetic content and throwaway keys only.
#include "maicord/app/update_changelog.hpp"
#include "maicord/app/update_fetch.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool success, const char* name) noexcept { test_check(&failures, success, name); }
bool same(Utf8View a, const char* b) noexcept {
    const u32 n = test_text_length(b);
    return a.bytes == n && bytes_equal(a.data, b, n);
}

constexpr char kAccepted[] =
    "{\"format\":1,\"product\":\"MaiCord\",\"channel\":\"preview\",\"version\":3004002,\"base\":3003999,"
    "\"entries\":[{\"version\":3004002,\"date\":1760000100,\"commit\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
    "\"text\":\"Fix \\\"quoted\\\" names\\nin the caf\xC3\xA9 list\"},"
    "{\"version\":3004001,\"date\":1760000000,\"commit\":\"\",\"text\":\"Add a theme picker\"}]}";

struct Fixture {
    KdfSha256Workspace workspace;
    P256Key signer, stranger;
    u32 scratch[kUpdateChangelogStructurals];
    UpdateChangelog log;
    char document[kUpdateChangelogMaximumBytes + 4096];
    u8 signature[64];
};
Fixture* g;

bool sign(Utf8View document, const P256Key& key, Utf8View domain, u8* signature) noexcept {
    u8 digest[32]{};
    const ConstSlice<u8> parts[]{{reinterpret_cast<const u8*>(domain.data), domain.bytes},
                                 {reinterpret_cast<const u8*>(document.data), document.bytes}};
    return sha256(g->workspace, {parts, 2}, {digest, 32}).ok() && p256_sign_digest(key, {digest, 32}, {signature, 64}).ok();
}
// Signs document with the publisher key under the changelog domain and verifies it.
UpdateChangelogError verify(Utf8View document) noexcept {
    if (!sign(document, g->signer, kUpdateChangelogSignatureDomain, g->signature)) return UpdateChangelogError::invalid_input;
    return update_changelog_verify(document, {g->signature, 64}, g->signer, g->workspace,
                                   {g->scratch, kUpdateChangelogStructurals}, &g->log);
}
// The accepted document with one substring replaced.
Utf8View edited(const char* find, const char* replace) noexcept {
    const Utf8View base{kAccepted, sizeof(kAccepted) - 1};
    const u32 f = test_text_length(find), r = test_text_length(replace);
    for (u32 i = 0; i + f <= base.bytes; ++i)
        if (bytes_equal(base.data + i, find, f)) {
            copy_bytes(g->document, base.data, i);
            copy_bytes(g->document + i, replace, r);
            copy_bytes(g->document + i + r, base.data + i + f, base.bytes - i - f);
            return {g->document, base.bytes - f + r};
        }
    return {};
}
// A document with count entries of text_bytes 'x' each.
Utf8View crowded(u32 count, u32 text_bytes) noexcept {
    u32 n = 0;
    const auto add = [&](const char* s) noexcept { const u32 b = test_text_length(s); copy_bytes(g->document + n, s, b); n += b; };
    add("{\"format\":1,\"product\":\"MaiCord\",\"channel\":\"stable\",\"version\":9,\"base\":0,\"entries\":[");
    for (u32 i = 0; i < count && n + text_bytes + 64 < sizeof(g->document); ++i) {
        if (i) add(",");
        add("{\"version\":9,\"date\":0,\"commit\":\"\",\"text\":\"");
        for (u32 j = 0; j < text_bytes; ++j) g->document[n++] = 'x';
        add("\"}");
    }
    add("]}");
    return {g->document, n};
}

void test_accepted() noexcept {
    const Utf8View accepted{kAccepted, sizeof(kAccepted) - 1};
    const auto& log = g->log;
    check(verify(accepted) == UpdateChangelogError::none && log.channel == UpdateChannel::preview && log.version == 3004002 &&
              log.base == 3003999 && log.count == 2 && log.entries[0].has_commit && !log.entries[1].has_commit &&
              log.entries[0].date_unix_seconds == 1760000100 && log.entries[0].commit[39] == 'a' &&
              same(update_changelog_text(log, log.entries[0]), "Fix \"quoted\" names\nin the caf\xC3\xA9 list") &&
              same(update_changelog_text(log, log.entries[1]), "Add a theme picker"),
          "changelog: the publisher's layout verifies and decodes escapes into entry text");
    // A manifest-domain signature, a stranger's key or one flipped byte fail.
    sign(accepted, g->signer, kUpdateManifestSignatureDomain, g->signature);
    const bool domain = update_changelog_verify(accepted, {g->signature, 64}, g->signer, g->workspace,
                                                {g->scratch, kUpdateChangelogStructurals}, &g->log) == UpdateChangelogError::signature;
    sign(accepted, g->stranger, kUpdateChangelogSignatureDomain, g->signature);
    const bool stranger = update_changelog_verify(accepted, {g->signature, 64}, g->signer, g->workspace,
                                                  {g->scratch, kUpdateChangelogStructurals}, &g->log) == UpdateChangelogError::signature;
    sign(accepted, g->signer, kUpdateChangelogSignatureDomain, g->signature);
    copy_bytes(g->document, kAccepted, accepted.bytes);
    g->document[40] ^= 1;
    const bool tampered = update_changelog_verify({g->document, accepted.bytes}, {g->signature, 64}, g->signer, g->workspace,
                                                  {g->scratch, kUpdateChangelogStructurals}, &g->log) == UpdateChangelogError::signature;
    check(domain && stranger && tampered && !g->log.count && !g->log.version,
          "changelog: only the publisher's changelog-domain signature over these exact bytes verifies");
}

void test_schema() noexcept {
    struct Case { const char* find; const char* replace; UpdateChangelogError expected; const char* name; };
    constexpr Case cases[]{
        {"\"format\":1", "\"format\":2", UpdateChangelogError::field, "format 2"},
        {"\"format\":1", "\"format\":1,\"format\":1", UpdateChangelogError::field, "duplicate field"},
        {"\"channel\":\"preview\"", "\"channel\":\"beta\"", UpdateChangelogError::field, "unknown channel"},
        {"\"product\":\"MaiCord\",", "", UpdateChangelogError::missing_field, "missing product"},
        {"\"base\":3003999", "\"base\":3004002", UpdateChangelogError::bounds, "base not below version"},
        {"\"version\":3004001", "\"version\":3004003", UpdateChangelogError::bounds, "entry above the previous one"},
        {"\"version\":3004001", "\"version\":3003999", UpdateChangelogError::bounds, "entry at the base"},
        {"\"commit\":\"\"", "\"commit\":\"abc\"", UpdateChangelogError::field, "short commit"},
        {"\"commit\":\"\"", "\"commit\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"", UpdateChangelogError::field, "uppercase commit"},
        {"\"text\":\"Add a theme picker\"", "\"text\":\"\"", UpdateChangelogError::bounds, "empty text"},
        {"\"text\":\"Add a theme picker\"}", "\"text\":\"Add\",\"extra\":0}", UpdateChangelogError::field, "unknown entry field"},
        {"\"date\":1760000000,", "", UpdateChangelogError::missing_field, "missing date"},
        {"\"entries\":[", "\"entries\":[1,", UpdateChangelogError::field, "non-object entry"},
    };
    bool all = true;
    for (const auto& c : cases) {
        const auto error = verify(edited(c.find, c.replace));
        if (error != c.expected || g->log.count) {
            test_write_line(c.name);
            all = false;
        }
    }
    check(all, "changelog: schema violations are refused with their reason and leave no entries");
    check(verify(crowded(100, 8)) == UpdateChangelogError::none && g->log.count == 100 &&
              verify(crowded(101, 8)) == UpdateChangelogError::bounds && !g->log.count,
          "changelog: at most 100 entries");
    check(verify(crowded(1, kUpdateChangelogTextMaximumBytes)) == UpdateChangelogError::none &&
              verify(crowded(1, kUpdateChangelogTextMaximumBytes + 1)) == UpdateChangelogError::bounds,
          "changelog: each entry text is at most 8192 bytes");
    check(verify(crowded(8, 8000)) == UpdateChangelogError::none &&
              g->log.text_bytes == 64000 && verify(crowded(8, 8150)) == UpdateChangelogError::invalid_input,
          "changelog: documents stay within 64 KiB");
}

void test_untrusted_text() noexcept {
    // Controls (escaped and raw C1), bidi overrides and tabs are cleaned; text
    // made only of controls is refused.
    const auto log = verify(edited("Add a theme picker",
                                   "A\\u0007d\\td\\u202e \\u0085x\xE2\x80\xAE\\r\\ny"));
    check(log == UpdateChangelogError::none && same(update_changelog_text(g->log, g->log.entries[1]), "Ad d x\ny"),
          "changelog: control characters and bidirectional overrides are dropped from entry text");
    check(verify(edited("Add a theme picker", "\\u0001\\u202e")) == UpdateChangelogError::bounds,
          "changelog: text with nothing printable is refused");
}

void test_display_text() noexcept {
    char out[512];
    constexpr char notes[] = "### Features\n\n- Add a [theme](https://example.invalid/x) picker\n* Show **bold** and `code`\n\n\n\n"
                             "### Fixes\n\n- Fix __it__\nPlain [unclosed link\n";
    const u32 n = update_changelog_display_text({notes, sizeof(notes) - 1}, {out, sizeof(out)});
    check(same({out, n}, "Features\n\n\xE2\x80\xA2 Add a theme picker\n\xE2\x80\xA2 Show bold and code\n\nFixes\n\n"
                         "\xE2\x80\xA2 Fix it\nPlain [unclosed link"),
          "changelog display: headings, bullets and links become plain text, blank runs collapse");
    const u32 short_n = update_changelog_display_text({"- caf\xC3\xA9", 7}, {out, 8});
    check(short_n == 7 && same({out, short_n}, "\xE2\x80\xA2 caf"), "changelog display: truncation never splits a character");
    char date[24];
    check(same({date, update_changelog_date_text(1760000100, true, {date, sizeof(date)})}, "Oct 9, 2025") &&
              same({date, update_changelog_date_text(1709208000, false, {date, sizeof(date)})}, "Feb 29") &&
              same({date, update_changelog_date_text(0, true, {date, sizeof(date)})}, "Jan 1, 1970") &&
              !update_changelog_date_text(1760000100, true, {date, 5}),
          "changelog display: dates read as abbreviated month, day and year (UTC)");
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_update_changelog_tests_entry() noexcept {
    using namespace maicord;
    g = static_cast<Fixture*>(VirtualAlloc(nullptr, sizeof(Fixture), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const bool ready = g && kdf_sha256_initialize(&g->workspace).ok() && p256_generate(&g->signer, P256Kind::ecdsa).ok() &&
                       p256_generate(&g->stranger, P256Kind::ecdsa).ok();
    check(ready, "changelog: throwaway publisher keys");
    if (ready) {
        test_accepted();
        test_schema();
        test_untrusted_text();
        test_display_text();
        p256_release(&g->signer);
        p256_release(&g->stranger);
        kdf_sha256_release(&g->workspace);
    }
    ExitProcess(failures ? 1 : 0);
}
