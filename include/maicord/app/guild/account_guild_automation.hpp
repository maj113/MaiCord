#pragma once
#include "maicord/app/net/rest_client.hpp"
#include "maicord/protocol/guild/automation.hpp"

namespace maicord {
// Integrations, webhooks, application command permissions and AutoMod rules
// on the authenticated DETAILS owner. Target and body are copied
// synchronously; the response is taken later through rest_take.
RestClientSubmitResult account_guild_automation_submit(RestClient*, const GuildAutomationRequest*,
                                                       u64 request_id) noexcept;
} // namespace maicord
