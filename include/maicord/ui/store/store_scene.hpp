#pragma once

#include "maicord/ui/core/paint.hpp"
#include "maicord/ui/core/widget_manager.hpp"
#include "maicord/ui/core/widget_storage.hpp"
#include "maicord/ui/widgets/icon.hpp"
#include "maicord/media/core/asset_key.hpp"

// The Plugins, Themes and Effects pages of Settings (store-ui.md): a retained tree of layout elements the host
// builds from its projection and the scene lays out, paints, hit-tests and scrolls. The scene knows nothing of
// plugins: an interactive element carries a host action id, and input comes back as UiStoreEvent. Text storage is
// the host's (elements borrow views) or the builder's arena. UI thread only.
namespace maicord {
struct Theme;
struct UiTextOutput;
struct UiEditView;

inline constexpr u32 kUiStoreNodeCapacity = 3072;
inline constexpr u32 kUiStoreTextBytes = 48 * 1024;

enum class UiStoreKind : u8 {
    // Containers.
    column,  // children stacked, stretched to the width
    row,     // children left to right at their own width; flex children share the rest
    flow,    // wrapping row (chips)
    grid,    // equal cells, columns from the width (min cell width in `w`)
    split,   // main (flex) + rail (`w` wide); stacked when narrow
    card,    // a plate around its children
    preview, // an image plate of a fixed aspect; children sit at the corners (UiStoreAnchor)
    // Leaves.
    text,
    markdown, // data = a retained UiMarkdown, text = its plain fallback
    icon,
    tile,
    chip,
    button,
    icon_button,
    toggle,  // the switch
    check,   // check box with a label
    filter,  // filter chip
    tab,
    search,
    dropdown,
    segment, // one option of a segmented track (the parent is a `segments` row)
    slider,
    swatch,
    bar,     // skeleton bar
    rule,
    spacer,
    disc,
    spinner
};

enum class UiStoreType : u8 {
    body,       // 16/20
    body_bold,
    small,      // 13/18
    small_bold,
    label,      // 14/18
    label_bold,
    eyebrow,    // 12/16 bold
    heading,    // 20/24 bold
    title,      // 24/30 bold
    mono,       // 13 mono
    paragraph,  // 16/22
    count
};

enum class UiStoreInk : u8 {
    strong, normal, subtle, header, link, warning, critical, positive, brand, on_brand, on_scrim
};
enum class UiStoreFill : u8 {
    none, subtle, normal, strong, input, selected, translucent, scrim, positive_tint, warning_tint, critical_tint,
    brand, critical, success, caution, danger
};
enum class UiStoreTone : u8 { neutral, subtle, risk_low, risk_medium, risk_high, positive, warning, dev, brand, mono, scrim };
enum class UiStoreButton : u8 { secondary, primary, ghost, critical, ghost_critical, link };
enum class UiStoreAnchor : u8 { none, top_left, top_right, bottom_left, bottom_right, center };
// live: a fixed-height plate the renderer fills with the running package effect (ui_background_paint_tile).
enum class UiStoreAspect : u8 { r16x10, r16x9, fixed, live };

enum UiStoreFlag : u16 {
    ui_store_flex = 1u << 0,      // takes the free space of a row
    ui_store_wrap = 1u << 1,      // a text wraps (else one line, ellipsized)
    ui_store_center = 1u << 2,    // text centred; a row centres its children vertically
    ui_store_end = 1u << 3,       // aligned to the end of its row
    ui_store_selected = 1u << 4,
    ui_store_disabled = 1u << 5,
    ui_store_busy = 1u << 6,
    ui_store_stretch = 1u << 7,   // a button or field fills its parent's width
    ui_store_foreground = 1u << 8,// drawn after the media layer (pills over previews)
    ui_store_critical = 1u << 10, // a critical emphasis (text, plate)
    ui_store_hairline = 1u << 11, // a card row is closed by a hairline
    ui_store_fallback = 1u << 12, // a dropdown shown in place of the segmented track before it when that doesn't fit
    ui_store_clip = 1u << 13,     // children are clipped to the rounded plate
    ui_store_hidden = 1u << 14,
    ui_store_justify = 1u << 15   // a row centres its children (no flex children)
};

// One element. Containers use pad_x/pad_y/gap; leaves their kind-specific fields.
struct UiStoreNode {
    UiStoreKind kind;
    u8 type;     // UiStoreType, button size (0 sm, 1 md), tile size class
    u8 ink;      // UiStoreInk
    u8 fill;     // UiStoreFill
    u8 variant;  // tone / button / aspect / swatch shape
    u8 icon;     // UiIcon, 255 none
    u8 icon2;
    u8 anchor;   // UiStoreAnchor in an overlay
    u16 flags;
    u16 first, next;
    u16 count;   // text: max lines; grid: columns (arranged); segments: option count
    u16 aux;     // slider: default index + 1
    u32 action;  // host action id; 0 is passive
    u32 value;   // selected index / checked / rgb / media slot / slider position
    u32 group;   // arrow-key group (0 none)
    float pad_x, pad_y, gap;
    float rw, rh; // requested size; 0 = intrinsic
    float x, y, w, h; // arranged, in content coordinates
    Utf8View text, text2;
    const void* data;
};
inline constexpr u16 kUiStoreNone = 0xffff;

// Builds the element tree into the scene's arrays: open/close for containers, leaf for the rest.
struct UiStoreScene;
class UiStoreBuilder {
  public:
    // dialog: builds the modal tree; bar: a non-modal bar floating at the bottom of the modal (no scrim).
    explicit UiStoreBuilder(UiStoreScene* scene, bool dialog = false, bool bar = false) noexcept;
    bool ok() const noexcept { return ok_; }
    // The element just added (valid until the next add).
    UiStoreNode& node() noexcept { return *current_; }
    UiStoreNode& open(UiStoreKind kind) noexcept;
    void close() noexcept;
    UiStoreNode& leaf(UiStoreKind kind) noexcept;
    // Common leaves. The short forms exist so a call site passes no stack arguments for defaults.
    UiStoreNode& text(const Utf8View& value, UiStoreType type = UiStoreType::body, UiStoreInk ink = UiStoreInk::normal) noexcept;
    UiStoreNode& text(const Utf8View& value, UiStoreType type, UiStoreInk ink, u16 flags) noexcept;
    UiStoreNode& chip(const Utf8View& value, UiStoreTone tone, UiIcon icon = UiIcon::count) noexcept;
    UiStoreNode& button(const Utf8View& value, UiStoreButton variant, u32 action) noexcept;
    UiStoreNode& button(const Utf8View& value, UiStoreButton variant, u32 action, UiIcon icon, bool large = false) noexcept;
    UiStoreNode& icon_button(UiIcon icon, const Utf8View& name, u32 action) noexcept; // 32 DIPs
    UiStoreNode& icon_button(UiIcon icon, const Utf8View& name, u32 action, float size) noexcept;
    UiStoreNode& tile(UiIcon icon, float size, UiStoreTone tone = UiStoreTone::neutral) noexcept;
    // Literal overloads: the caller passes the C string alone and the builder counts it, so a call site is
    // smaller than one that builds a Utf8View.
    UiStoreNode& text(const char* value, UiStoreType type = UiStoreType::body, UiStoreInk ink = UiStoreInk::normal) noexcept;
    UiStoreNode& text(const char* value, UiStoreType type, UiStoreInk ink, u16 flags) noexcept;
    UiStoreNode& chip(const char* value, UiStoreTone tone, UiIcon icon = UiIcon::count) noexcept;
    UiStoreNode& button(const char* value, UiStoreButton variant, u32 action) noexcept;
    UiStoreNode& button(const char* value, UiStoreButton variant, u32 action, UiIcon icon, bool large = false) noexcept;
    UiStoreNode& icon_button(UiIcon icon, const char* name, u32 action) noexcept;
    UiStoreNode& icon_button(UiIcon icon, const char* name, u32 action, float size) noexcept;
    UiStoreNode& spacer(float width, float height, bool flex = false) noexcept;
    // Copies text into the scene's arena (bytes beyond capacity drop the piece). pieces are concatenated.
    Utf8View copy(const Utf8View& a) noexcept;
    Utf8View copy(const Utf8View& a, const Utf8View& b) noexcept;
    Utf8View copy(const Utf8View& a, const Utf8View& b, const Utf8View& c) noexcept;
    Utf8View copy(const Utf8View& a, const Utf8View& b, const Utf8View& c, const Utf8View& d) noexcept;
    Utf8View number(u64 value) noexcept;
    // Restores the failure state; the tree stays empty on failure.
    void fail() noexcept { ok_ = false; }

  private:
    UiStoreNode& add(UiStoreKind kind) noexcept;
    Utf8View gather(const Utf8View* const* parts, u32 count) noexcept;
    UiStoreScene* scene_;
    UiStoreNode* current_;
    u16 stack_[24];
    u32 depth_;
    u16 last_;
    bool ok_;
};

enum class UiStoreEventKind : u8 { none, activate, value };
struct UiStoreEvent {
    UiStoreEventKind kind;
    u32 action;
    u32 value;
    UiRect anchor; // a menu or dropdown opens here (viewport coordinates)
};

// Host-owned editor of the search field (text, caret and IME stay with the host).
struct UiStoreSearch {
    const UiEditView* view;
    const struct UiEditGeometry* geometry;
    Utf8View placeholder;
    u32 action;
};

struct UiStoreMedia {
    MediaAssetKey key;
    u32 slot;        // the preview node's value
    u8 domain;       // 0: the media atlas, 1: the local preview atlas
    UiRect rect, clip, mask;
    float mask_radius;
};

// Text widths come from the host's measuring cache when it has one; without it widths are estimated.
struct UiStoreMeasure {
    float (*width)(void* context, const Utf8View& text, u32 style) noexcept;
    void* context;
};
struct UiTextMeasureCache;
// A measure over the shell's text cache (shaped widths).
UiStoreMeasure ui_store_cache_measure(UiTextMeasureCache* cache) noexcept;

struct UiStoreScene {
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    UiWidgetHandle root;
    UiStoreNode* nodes;
    char* text;
    u32 count, text_used;
    // Roots of the page tree and the modal dialog tree (kUiStoreNone: none); dialog nodes follow the page's.
    u16 page_root, dialog_root, dialog_footer;
    // Geometry given by commit(): `panel` is the whole panel body (the scroll clip), `column` the content
    // column, `modal` the settings modal the dialog centres in.
    UiRect panel, column, modal, dialog_rect;
    float scroll, content_height, dialog_scroll, dialog_height;
    u32 revision;   // content, layout and scroll: text must be rebuilt
    u32 motion;     // hover, press, focus and animation: paint only
    u32 hover_action, focus_action, press_action;
    bool focus_visible, active, dialog_open, dialog_bar, reduced_motion;
    bool hover_clear; // the pointer is over the clear button of the hovered search field
    u64 clock_ms;
    // Host-sampled UiSpinnerClock phase (busy buttons, the dialog's spinner); part of the paint stamp.
    u8 spinner_phase;
    const UiStoreSearch* search;
    const UiStoreMeasure* measure;
    // Resolves a preview's slot and its rectangle (DIPs) to its media identity; false: no picture for it. The
    // renderer queues the picture when it is resident.
    const void* media_context;
    bool (*media_key)(const void* context, u32 slot, const UiRect& rect, MediaAssetKey* key, u8* domain) noexcept;
};

// Lifecycle. The arrays are allocated once while a store page is open.
bool ui_store_scene_open(UiStoreScene*) noexcept;
void ui_store_scene_release(UiStoreScene*) noexcept;
// Starts a rebuild: clears the nodes and the arena.
void ui_store_scene_begin(UiStoreScene*) noexcept;
// Ends a rebuild and lays the tree out in `column` (page) and `modal` (dialog); keeps scroll and focus.
bool ui_store_scene_commit(UiStoreScene*, const UiRect& panel, const UiRect& column, const UiRect& modal) noexcept;

// Input. Coordinates are viewport DIPs.
bool ui_store_scene_hover(UiStoreScene*, float x, float y) noexcept;
bool ui_store_scene_press(UiStoreScene*, float x, float y, UiStoreEvent*) noexcept;
bool ui_store_scene_release_pointer(UiStoreScene*, float x, float y, UiStoreEvent*) noexcept;
// `delta` moves the open dialog's body, else the page.
bool ui_store_scene_scroll(UiStoreScene*, float delta) noexcept;
bool ui_store_scene_scroll_to(UiStoreScene*, float offset) noexcept;
// The page's (or the open dialog body's) scroll viewport, offset and largest offset; false when it has none.
bool ui_store_scene_scroll_range(const UiStoreScene&, bool dialog, UiRect* viewport, float* offset, float* most) noexcept;
enum class UiStoreKey : u8 { left, right, up, down, home, end, page_up, page_down, first, last, enter, space, menu, tab, back_tab };
// Keyboard: returns whether the key was used; *event reports an activation.
bool ui_store_scene_key(UiStoreScene*, UiStoreKey, UiStoreEvent*) noexcept;
bool ui_store_scene_focus(UiStoreScene*, u32 action, bool visible) noexcept;
u32 ui_store_scene_focused(const UiStoreScene&) noexcept;
bool ui_store_scene_slider(UiStoreScene*, u32 action, float x, UiStoreEvent*) noexcept;
bool ui_store_scene_rect(const UiStoreScene&, u32 action, UiRect*) noexcept;
// The pressed node is a slider (a drag follows the pointer until release); a node's current value.
bool ui_store_scene_press_is_slider(const UiStoreScene&) noexcept;
u32 ui_store_scene_value(const UiStoreScene&, u32 action) noexcept;
// True when the pointer is over a dialog (so the page must not react).
bool ui_store_scene_modal_hit(const UiStoreScene&, float x, float y) noexcept;
// The skeleton pulse: advances the paint clock; true while a pulsing skeleton is on screen (a frame is due).
bool ui_store_scene_tick(UiStoreScene*, u64 now_ms, bool reduced_motion) noexcept;
bool ui_store_scene_animating(const UiStoreScene&) noexcept;
// True while a spinner (a busy button, the dialog's check) is on screen: the host's shared spinner clock drives it.
bool ui_store_scene_spinner_visible(const UiStoreScene&) noexcept;

// Painting: layer 0 = page plates, 1 = foreground (pills over previews), 2 = dialog plates and its foreground.
bool ui_store_scene_paint(const UiStoreScene&, const Theme&, PaintStream*, u32 layer) noexcept;
bool ui_store_scene_text_output(const UiStoreScene&, const Theme&, UiTextOutput*, bool dialog) noexcept;
// Visible preview tiles, in order; cursor starts at 0.
bool ui_store_scene_next_media(const UiStoreScene&, u32* cursor, UiStoreMedia*) noexcept;
// A live tile of the page is on screen (not scrolled out, no dialog over it): the effect it shows may animate.
bool ui_store_scene_live_visible(const UiStoreScene&) noexcept;
// Where the live search editor's text goes (the node of search->action), in viewport coordinates.
bool ui_store_scene_search_geometry(const UiStoreScene&, UiRect* text) noexcept;

static_assert(sizeof(UiStoreNode) <= 112);
// The interactive nodes as accessibility nodes (buttons, cards, switches, tabs, fields, sliders), in tree order; the
// name of a control without a label is the nearest title before it. Returns the first node added.
struct UiAccessTree;
u32 ui_access_add_store(UiAccessTree* tree, u32 parent, const UiStoreScene& scene, u8 surface) noexcept;

} // namespace maicord
