#pragma once

#include "maicord/app/link_trust.hpp"

namespace maicord::app_detail {

// Opens an external link by the official client's rules: trusted links launch at once,
// others show the "Leaving Discord" warning with a "Trust <host> links from
// now on" checkbox that is saved per account. trusted_text: the link's text
// is its URL and it is not in a DM with someone who is not a friend.
bool application_link_open(Utf8View url, bool trusted_text) noexcept;
// The official client's markdown rule for a link in this channel: false in a one-to-one
// DM whose recipient is not a friend.
bool application_link_channel_trusted(u64 channel_id) noexcept;
// The current account's trusted hosts and protocols, loaded on first use.
const LinkTrustStore* application_link_trust_store() noexcept;

} // namespace maicord::app_detail
