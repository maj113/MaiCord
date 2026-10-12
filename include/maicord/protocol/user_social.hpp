#pragma once
#include "maicord/core/arena.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/rest.hpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
// User notes, in-app reports (the menu-driven reporting API) and message flag
// edits. Targets and bodies match the official client field for field.
inline constexpr u32 kUserNoteMaximumCodepoints = 256;
inline constexpr u32 kUserNoteMaximumBytes = kUserNoteMaximumCodepoints * 4;
inline constexpr u32 kUserSocialMaximumTargetBytes = 128;
inline constexpr u32 kUserSocialMaximumBodyBytes = 16 * 1024;
inline constexpr u64 kUserSocialRouteBase = 0x534F4349414C0000ull; // "SOCIAL"
inline constexpr u32 kMessageFlagSuppressEmbeds = 1u << 2;

enum class UserSocialOperation : u8 {
    note_get,      // GET   /users/@me/notes/{user}
    note_put,      // PUT   /users/@me/notes/{user} {"note":...}
    report_menu,   // GET   /reporting/menu/{name}
    report_submit, // POST  /reporting/{name}; text is report_write_submit's body
    message_flags, // PATCH /channels/{c}/messages/{m} {"flags":N}
    count
};
enum class ReportMenuName : u8 { message, user, count };

// Crosses the account module ABI (append-only).
struct UserSocialRequest {
    UserSocialOperation operation;
    ReportMenuName report;
    u16 reserved;
    u32 flags;                  // message_flags: the complete new flags value
    u64 user_id;                // note_get / note_put
    u64 channel_id, message_id; // message_flags
    Utf8View text;              // note_put: the note (empty clears); report_submit: JSON body
};
static_assert(sizeof(UserSocialRequest) == 48);

enum class UserSocialWriteError : u8 { none, input, capacity };
struct UserSocialWriteResult {
    u32 bytes;
    UserSocialWriteError error;
    constexpr bool ok() const noexcept {
        return error == UserSocialWriteError::none;
    }
};

Utf8View report_menu_name(ReportMenuName) noexcept;
bool user_note_valid(Utf8View note) noexcept;
bool user_social_request_valid(const UserSocialRequest&) noexcept;
RestMethod user_social_method(UserSocialOperation) noexcept;
bool user_social_has_body(UserSocialOperation) noexcept;
RestRouteKey user_social_route(const UserSocialRequest&) noexcept;
// Writers leave output untouched on failure.
UserSocialWriteResult user_social_write_target(const UserSocialRequest&, Slice<u8> output) noexcept;
UserSocialWriteResult user_social_write_body(const UserSocialRequest&, Slice<u8> output) noexcept;

// GET note response ({"note_user_id","note"}) or USER_NOTE_UPDATE data
// ({"id","note"}). The note is copied; a missing or null note is empty.
struct UserNoteRecord {
    u64 user_id;
    u32 bytes;
    char text[kUserNoteMaximumBytes];
};
bool user_note_decode(Utf8View body, Slice<u32> scratch, bool gateway, UserNoteRecord* output) noexcept;

// GET /reporting/menu/{name}: a node tree. Each node shows a header, optional
// subheader/info, elements (text blocks, checkbox groups, free text), child
// options leading to other nodes and an optional button (next/submit/done/cancel).
inline constexpr u32 kReportMaximumNodes = 96;
inline constexpr u32 kReportMaximumChildren = 512;
inline constexpr u32 kReportMaximumElements = 384;
inline constexpr u32 kReportMaximumOptions = 768;
inline constexpr u32 kReportMaximumBreadcrumbs = 32;
inline constexpr u32 kReportFreeTextMaximumBytes = 1024;

enum class ReportElementKind : u8 {
    unknown,
    checkbox,      // data: [[value, label, description?], ...]; a multi-select group
    text,          // data: {header, body} or a string
    free_text,     // a text input submitted under its name
    external_link, // data: {url, link_text, link_description?}
    breadcrumbs,
    message_preview,
    user_preview,
    success,
    fail,
};
enum class ReportButtonKind : u8 { none, next, submit, done, cancel };
struct ReportOption {
    Utf8View value, label, description;
};
struct ReportElement {
    Utf8View name, header, body, url;
    u32 option_first, option_count;
    ReportElementKind kind;
    bool submit; // should_submit_data
};
struct ReportChild {
    Utf8View label;
    u32 target;
};
struct ReportNode {
    u32 id;
    Utf8View key, header, subheader, info;
    ReportButtonKind button;
    u32 button_target;
    u32 child_first, child_count, element_first, element_count;
    bool multi_select_required, auto_submit;
};
struct ReportMenu {
    Arena storage;
    JsonIndexedDocument document;
    Utf8View name, language;
    // Echoed verbatim (string or number) in the submission.
    Utf8View version_json, variant_json;
    u32 root_node_id, success_node_id, fail_node_id;
    ReportNode* nodes;
    ReportChild* children;
    ReportElement* elements;
    ReportOption* options;
    u32 node_count, child_count, element_count, option_count;
};
bool report_menu_decode(Utf8View body, Slice<u32> scratch, ReportMenu* output) noexcept;
void report_menu_release(ReportMenu*) noexcept;
const ReportNode* report_menu_node(const ReportMenu&, u32 id) noexcept;

// The visited path and the answers given so far. Checkbox selections are
// bitmasks over each element's options; free text is one value per element.
struct ReportAnswer {
    u32 element; // index into ReportMenu::elements
    u64 selected;
    Utf8View text;
};
struct ReportSubmission {
    ReportMenuName name;
    u64 channel_id, message_id; // message reports
    u64 user_id, guild_id;      // user reports; guild is the contextual server (optional)
    ConstSlice<u32> breadcrumbs;
    ConstSlice<ReportAnswer> answers;
};
// {"version","variant","language","breadcrumbs","elements",ids...,"name"} in
// the official key order; unselected groups are omitted.
UserSocialWriteResult report_write_submit(const ReportMenu&, const ReportSubmission&, Slice<u8> output) noexcept;
} // namespace maicord
