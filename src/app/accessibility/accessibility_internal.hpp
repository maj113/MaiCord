#pragma once
// Shared by accessibility.cpp and its parts (accessibility_*.cpp); private to them.
#include "maicord/core/memory.hpp"

#include "../chat/composer/attachments_state.hpp"
#include "../shell/state.hpp"
#include "../../render/d3d11/renderer_interaction_state.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/platform/uia/provider.hpp"
#include "maicord/render/text/forum_post_surface.hpp"
#include "maicord/render/text/form_surface.hpp"
#include "maicord/ui/accessibility/scenes.hpp"
#include "maicord/ui/text/secure_edit.hpp"
#include "../account/account_ui_runtime.hpp"

#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace accessibility_detail {}
using namespace accessibility_detail;
namespace accessibility_detail {

// Surfaces tag nodes so deferred actions reach the owning controller.
enum AccessSurface : u8 {
    surface_shell = 1,
    surface_guilds,
    surface_channels,
    surface_messages,
    surface_composer,
    surface_members,
    surface_popup,
    surface_settings,
    surface_login,
    surface_friends,
    // Overlays (accessibility_scenes).
    surface_quick,
    surface_search,
    surface_pins,
    surface_threads,
    surface_inbox,
    surface_call,
    surface_forum,
    surface_form,
    surface_viewer,
    surface_picker,
    surface_caption,
    surface_profile,
};

constexpr u32 kNodeCapacity = 8192;
constexpr u32 kTextCapacity = 768u * 1024u;
constexpr u32 kSlotCapacity = 65536;
constexpr u32 kKeyIndexCapacity = 16384;
constexpr u32 kObservationCapacity = 4096;
// Nodes, slots and text are reserved at the capacities above and committed on
// demand: from these sizes, doubling when a snapshot exhausts a region.
constexpr u32 kInitialNodeCapacity = 1024;
constexpr u32 kInitialTextCapacity = 64u * 1024u;
constexpr u32 kInitialSlotCapacity = 8192;
constexpr u32 kPendingCapacity = 8;
// Deferred snapshot rebuild after coalesced animation frames.
constexpr UINT_PTR kRebuildTimer = 0x4D414343u;
// UiaClientsAreListening is sampled, not called per frame.
constexpr u64 kListenSampleMs = 250;
constexpr u64 kMentionSampleMs = 1000;

struct PendingAction {
    u64 key;
    double number;
    UiAccessAction action;
};

// Status sources observed between published snapshots for announcements.
struct UploadObservation {
    u64 nonce;
    u32 generation;
    MessageAttachmentDraftPhase phase;
    bool occupied;
};

struct StatusObserver {
    UploadObservation uploads[kMessageAttachmentDraftCapacity];
    const void* account_instance;
    u64 voice_channel;
    u64 failed_channel;
    u64 mention_total;
    u64 mention_sampled_ms;
    u64 quick_summary, search_summary;
    const char* connection;
    u32 failed_count;
    u32 toast_serial;
    MessageComposerPhase composer_phase;
    bool voice_connected;
    bool primed, mentions_primed;
};

struct AccessibilityRuntime {
    UiaProviderHost host;
    UiAccessTree tree;
    UiAccessEventState events;
    UiAccessMessageCursor messages;
    UiAccessThrottle throttle;
    UiAccessAnnouncer announcer;
    StatusObserver status;
    void* storage;
    PendingAction pending[kPendingCapacity];
    char pending_text[kUiAccessValueMaximumBytes];
    u64 listen_sampled_ms;
    u32 pending_text_bytes;
    u32 pending_count;
    bool initialized;
    bool dirty;
    bool posted;
    bool shut_down;
    bool listening;
    bool timer;
};

extern AccessibilityRuntime g_access;

// Describers compose names here; ui_access_add copies them immediately.
using NameBuilder = UiAccessNameBuilder;

extern NameBuilder g_name;
float access_scale(void*) noexcept;
UiPoint center(const UiRect& rectangle) noexcept;
const ChatStateStore* chat_state() noexcept;
Utf8View channel_name(u64 channel_id) noexcept;
void build_snapshot() noexcept;
bool ensure_storage() noexcept;
const UiAccessTree* access_tree(void*) noexcept;
bool access_act(void*, u64 key, UiAccessAction action, double number, Utf8View text) noexcept;
void run_action(const PendingAction& pending) noexcept;

} // namespace accessibility_detail
} // namespace maicord::app_detail
