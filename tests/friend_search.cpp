#include "maicord/model/friend_search.hpp"

namespace maicord {
namespace {
bool matches(Utf8View query, Utf8View username, Utf8View nickname = {}, Utf8View display = {}) noexcept {
    const auto result = friend_search(query, username, nickname, display);
    return result.ok() && result.matched;
}
bool ordered(Utf8View left, Utf8View right, i32 expected) noexcept {
    const auto result = friend_name_compare(left, right);
    return result.ok() && result.order == expected;
}
}
void test_friend_search(void (*check)(bool, const char*) noexcept) noexcept {
    check(matches(utf8_literal("ACCOUNT"), utf8_literal("account.name"), utf8_literal("Local Alias"), utf8_literal("Global Person")) &&
        matches(utf8_literal("alias"), utf8_literal("account.name"), utf8_literal("Local Alias"), utf8_literal("Global Person")) &&
        matches(utf8_literal("PERSON"), utf8_literal("account.name"), utf8_literal("Local Alias"), utf8_literal("Global Person")),
        "friend search: username, relationship nickname and global display name each match independently of displayed-name precedence");
    check(matches({}, {}) && matches({}, utf8_literal("ordinary")) &&
        !matches(utf8_literal("missing"), {}) && !matches(utf8_literal("account alias"), utf8_literal("account"), utf8_literal("alias")),
        "friend search: empty query matches valid unnamed input while fields are separate substrings, not joined search tokens");
    check(matches(utf8_literal("real name"), {}, utf8_literal("A Real Name Here")) &&
        !matches(utf8_literal("real  name"), {}, utf8_literal("A Real Name Here")) &&
        !matches(utf8_literal(" real"), utf8_literal("Real")) && !matches(utf8_literal("real "), utf8_literal("Real")),
        "friend search: multiword substring retains whitespace without trimming, token splitting or fuzzy matching");
    check(matches(utf8_literal("i\xCC\x87"), {}, {}, utf8_literal("\xC4\xB0")) &&
        matches(utf8_literal("\xC4\xB0"), utf8_literal("i\xCC\x87")) &&
        matches(utf8_literal("I"), utf8_literal("i")) &&
        !matches(utf8_literal("\xC4\xB1"), utf8_literal("I")),
        "friend search: default Unicode dotted-I expansion is locale independent and never applies Turkish dotless-I tailoring");
    check(matches(utf8_literal("\xCE\xBF\xCF\x82"), utf8_literal("\xCE\x9F\xCE\xA3")) &&
        !matches(utf8_literal("\xCE\xBF\xCF\x83"), utf8_literal("\xCE\x9F\xCE\xA3")) &&
        !matches(utf8_literal("\xCE\x9F\xCE\xA3"), utf8_literal("\xCE\x9F\xCE\xA3\xCE\x91")) &&
        matches(utf8_literal("\xCF\x83"), utf8_literal("\xCE\xA3")),
        "friend search: Greek final sigma uses original full-string context independently for query and name");
    check(matches(utf8_literal("\xCF\x82"), utf8_literal("A\xCC\x81\xCE\xA3")) &&
        !matches(utf8_literal("\xCF\x82"), utf8_literal("A\xCE\xA3'B")) &&
        matches(utf8_literal("\xCF\x83"), utf8_literal("\xCD\x85\xCE\xA3")) &&
        matches(utf8_literal("\xCF\x82"), utf8_literal("\xC2\xAA\xCE\xA3")),
        "friend search: sigma skips Case_Ignorable combining marks/apostrophe and honors derived Cased beyond ordinary letter categories");
    check(matches(utf8_literal("\xF0\x90\x90\xA8"), utf8_literal("\xF0\x90\x90\x80")) &&
        matches(utf8_literal("\xF0\x9E\xA4\xA2"), utf8_literal("\xF0\x9E\xA4\x80")),
        "friend search: supplementary Deseret and Adlam lowercase preserve whole Unicode scalars");
    check(matches(utf8_literal("\xC3\xA9"), utf8_literal("CAF\xC3\x89")) &&
        !matches(utf8_literal("cafe"), utf8_literal("CAF\xC3\x89")) &&
        !matches(utf8_literal("\xC3\xA9"), utf8_literal("e\xCC\x81")) &&
        !matches(utf8_literal("ss"), utf8_literal("\xC3\x9F")),
        "friend search: accents remain significant and full lowercase does not add normalization or case-folding sharp-S expansion");
    char maximum[129];for (auto& byte : maximum) byte = 'A';
    check(matches({maximum, 128}, {maximum, 128}) &&
        friend_search({maximum, 129}, {}, {}, {}).error == FriendSearchError::invalid_query &&
        friend_search({}, {maximum, 129}, {}, {}).error == FriendSearchError::invalid_name,
        "friend search: 128-byte query/name bounds are inclusive and oversized input fails before decoding");
    char dotted[128];for (u32 i = 0; i < 128; i += 2) {dotted[i] = static_cast<char>(0xC4);dotted[i + 1] = static_cast<char>(0xB0);}
    check(matches({dotted, sizeof(dotted)}, {dotted, sizeof(dotted)}),
        "friend search: maximum bounded dotted-I input expands safely without allocation or scalar-capacity overflow");
    const char invalid[]{static_cast<char>(0xC0), static_cast<char>(0xAF)};
    const char surrogate[]{static_cast<char>(0xED), static_cast<char>(0xA0), static_cast<char>(0x80)};
    check(friend_search({invalid, 2}, {}, {}, {}).error == FriendSearchError::invalid_query &&
        friend_search({surrogate, 3}, {}, {}, {}).error == FriendSearchError::invalid_query &&
        friend_search({nullptr, 1}, {}, {}, {}).error == FriendSearchError::invalid_query &&
        friend_search({}, {invalid, 2}, {}, {}).error == FriendSearchError::invalid_name &&
        friend_search(utf8_literal("match"), utf8_literal("match"), {invalid, 2}, {}).error == FriendSearchError::invalid_name,
        "friend search: malformed UTF-8, surrogate and missing-pointer errors remain explicit even for empty query or an earlier matched field");
    const auto tie = friend_name_compare(utf8_literal("ALICE"), utf8_literal("alice"));
    check(tie.ok() && !tie.order && ordered({}, utf8_literal("a"), -1) &&
        ordered(utf8_literal("prefix-more"), utf8_literal("prefix"), 1) &&
        ordered(utf8_literal("bob"), utf8_literal("alice"), 1),
        "friend name order: lowercase ties leave identity to caller and prefix/ordinary names compare lexically");
    check(ordered(utf8_literal("\xC4\xB0"), utf8_literal("i\xCC\x87"), 0) &&
        ordered(utf8_literal("\xCE\x9F\xCE\xA3"), utf8_literal("\xCE\xBF\xCF\x82"), 0) &&
        ordered(utf8_literal("\xC3\xA9"), utf8_literal("e\xCC\x81"), 1),
        "friend name order: shared full lowercase honors dotted-I/sigma and retains non-normalized accent distinctions");
    const auto supplementary = utf8_literal("\xF0\x90\x80\x80");
    const auto bmp = utf8_literal("\xEE\x80\x80");
    check(ordered(supplementary, bmp, -1) && ordered(bmp, supplementary, 1) &&
        ordered(utf8_literal("\xF0\x90\x90\x80"), utf8_literal("\xF0\x90\x90\x81"), -1),
        "friend name order: supplementary UTF-16 high/low units sort correctly before BMP private-use boundary unlike scalar or UTF-8 order");
    check(friend_name_compare({invalid, 2}, {}).error == FriendSearchError::invalid_name &&
        friend_name_compare({}, {maximum, 129}).error == FriendSearchError::invalid_name &&
        friend_name_compare({maximum, 128}, {maximum, 128}).ok() &&
        friend_name_compare({maximum, 128}, {maximum, 128}).order == 0,
        "friend name order: bounded invalid input returns explicit error and valid maximum names preserve equality");
}
}
