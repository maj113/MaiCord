# MaiCord plugin SDK (ABI 6)

A MaiCord plugin is a native x64 DLL with no entry point whose only export is
one `const` descriptor, `maicord_plugin_v6`. The descriptor declares
everything the plugin does: metadata, permissions, settings, state, hooks,
slash commands, context-menu items, buttons and the HTTPS hosts it may reach.
Loading a plugin runs none of its code; the host checks the PE and the
descriptor first.

- `plugin_abi.h` is the C ABI (C11 or C++). It's the reference for every
  struct, constant and host function.
- `plugin.hpp` is the C++23 authoring layer: one `.cpp` file of declarations
  becomes the descriptor at compile time. No STL, CRT, exceptions or RTTI.
- `samples/plugins/` has working plugins: `clear-urls` (hooks),
  `translate` (network, buttons, cards, a command, state), `show-me-your-name`
  (user decorations), `reverse-image-search` (a submenu) and `silent-toggle`
  (a toggle button and a command).

Plugins are trusted native code: the host attributes crashes and hangs and
turns misbehaving plugins off, but it isn't a sandbox.

## A plugin in C++

```cpp
#include <maicord/sdk/plugin.hpp>
using namespace mc;

MC_SETTINGS((Toggle, loud, "Shout", false));
struct State { u32 sent; };
MC_STATE(State, .version = 1);

static void pre_send(Ctx& ctx, Draft& draft) {
    ++ctx.state<State>().sent;
    if (ctx.settings().loud) draft.replace("HELLO");
}
MC_HOOKS(pre_send);

MC_COMMAND(count, (), "Show how many messages you sent") {
    Text<32> text;
    text.append("Sent: ");
    // ... format ctx.state<State>().sent ...
    return Reply::local(text);
}

MC_PLUGIN(.owner = 0x1234'5678'9ABC'DEF0, .name = "Example", .version = "1.0.0",
          .summary = "Shows what a plugin looks like.");
```

Rules:
- Use the macros at global scope. `MC_SETTINGS`, `MC_STATE`, `MC_HOOKS` and
  `MC_PLUGIN` appear at most once; `MC_PLUGIN` comes last.
- Designated options follow declaration order (see the structs in `plugin.hpp`).
- Define `MC_RUNTIME` in the one translation unit, so `plugin.hpp` provides
  `memcpy`, `memset`, `memmove`, `memcmp`, `__chkstk` and `_fltused`.
- Every entry point gets a `Ctx&` that is valid until it returns. Strings the
  host passes in are valid for the call; copy what you keep.
- State (`MC_STATE`) must be trivially copyable and hold no pointers. It's
  zeroed at first start and survives a hot reload while its size, alignment
  and `.version` stay the same.

Building with clang-cl and lld-link (`<include>` holds `maicord/sdk/`):

```powershell
clang-cl /nologo /c /std:c++latest /O2 /W4 /WX /utf-8 /Zl /GS- /GR- /DMC_RUNTIME /I<include> plugin.cpp
lld-link /nologo /DLL /NOENTRY /NODEFAULTLIB /OPT:REF /OPT:ICF plugin.obj /OUT:my-plugin.dll
```

## Descriptor (`MC_PLUGIN`)

| Option | Meaning |
|---|---|
| `.owner` | Non-zero 64-bit id; names the plugin and its data |
| `.name`, `.version` | 1..64 and 1..32 bytes |
| `.summary`, `.author`, `.homepage` | Optional; the homepage is `https://` |
| `.permissions` | `MC_PERM_*` bits the API calls need (below) |
| `.network` | Up to 8 HTTPS hosts: `"api.example.com"` or `"*.example.com"` |
| `.api_level` | Defaults to the header's `MC_API_LEVEL` |
| `.start`, `.stop` | `void (Ctx&)`; optional |

## Permissions

The host adds the permissions hooks, buttons and hosts imply, so declare only
what API calls need. The C++ layer makes a missing permission a compile error
(`ctx.copy needs .permissions = MC_PERM_CLIPBOARD`).

| Permission | Implied by / needed for |
|---|---|
| `MC_PERM_MESSAGE_SEND` | `PRE_SEND`, `PRE_EDIT`, `PRE_TYPING` hooks |
| `MC_PERM_READ_MESSAGES` | `MESSAGE_CLICK`, `DECORATE_MESSAGE`, message events, popover buttons |
| `MC_PERM_MESSAGE_HISTORY` | `MESSAGE_DELETED`, `MESSAGE_EDITED` (keep them visible this session) |
| `MC_PERM_READ_ACTIVITY` | Presence, voice, server, member and friend events |
| `MC_PERM_NETWORK` | A non-empty `.network`; `ctx.http` |
| `MC_PERM_COMPOSER` | `ctx.composer.*` |
| `MC_PERM_ACT_MESSAGES` | `ctx.send`, `edit_message`, `react`, `delete_message`, `mark_read` |
| `MC_PERM_ACT_PRESENCE` | `ctx.set_activity` |
| `MC_PERM_CLIPBOARD` | `ctx.copy` |
| `MC_PERM_AUTOMATED_ACTIONS` | Writes outside a gesture (implies `ACT_MESSAGES`) |
| `MC_PERM_PERSISTENT_LOG` | `ctx.kept.restore` (re-seeding kept messages from your own log) |

The last two let a plugin act on its own or keep deleted messages across
restarts. The official catalog refuses them; sideloaded plugins can use them
after the user agrees to a separate consent line.

**Gestures.** Clicks on your buttons, menu items, card buttons and commands
are gestures (`ctx.gesture()`). A completion of a call started inside a
gesture keeps it for 5 seconds. Writes need a gesture unless the plugin has
`MC_PERM_AUTOMATED_ACTIONS`; `open_url`, `copy`, `open_*` and `modal` always
need one. Writes are rate-limited (`MC_E_RATE_LIMITED`).

## Settings (`MC_SETTINGS`)

Each row is `(Type, field, "Label", default, options...)`. The host shows an
automatic settings page and writes the values into `mc::Settings`, which
handlers read with `ctx.settings().field` (plain loads). `ctx.set_setting(
Setting::field, value)` changes one from the plugin; the new value shows from
the next call. `SETTING_CHANGED` reports user changes.

| Type | Field | Default | Options |
|---|---|---|---|
| `Toggle` | `bool` | `true`/`false` | |
| `Int` | `i32` | number | `.min .max` |
| `Slider` | `i32` | number | `.min .max .step` |
| `Choice` | `u32` index | index | `.items = {"A", "B"}` |
| `Text` | `Str` | `"text"` or `{}` | `.hint .max_bytes` |
| `Secret` | `Str` | `{}` | `.hint .max_bytes`; stored sealed (DPAPI), shown masked |
| `TextList` | `StrList` | `{"a", "b"}` or `{}` | `.hint .max_bytes` |
| `Colour` | `u32` `0xRRGGBB` | colour | |
| `Keybind` | `u32` `MC_KEY(vk, MC_KEY_CTRL...)` | key or 0 | fires `KEYBIND` |
| `Channel` | `u64` channel id | `{}` | |
| `UserList` | `IdList` | `{}` | `.contains(id)` |
| `Header` | (nothing) | omitted | a section label |

Every row also takes `.description`, `.flags` (`MC_SET_HIDDEN`, `MC_SET_DANGER`,
`MC_SET_MULTILINE`), `.visible_if = Setting::toggle`, or `.visible_if =
Setting::choice, .visible_value = index`.

## Hooks (`MC_HOOKS`)

List plain functions; their parameter types pick the hook. Wrap one in
`mc::filter<fn, bits>` to set its filter.

| Signature | Hook | Notes |
|---|---|---|
| `void (Ctx&, Draft&)` | `PRE_SEND` | `replace`, `cancel`, `confirm(question)`, `set_flag(MC_DRAFT_SILENT...)` |
| `void (Ctx&, EditDraft&)` | `PRE_EDIT` | |
| `bool (Ctx&, const Message&, const Click&)` | `MESSAGE_CLICK` | true = handled; filter `MC_CLICK_FILTER_*` |
| `bool (Ctx&, const DeletedMessage&)` | `MESSAGE_DELETED` | true = keep a tombstone |
| `bool (Ctx&, const Message& before, const Message& after)` | `MESSAGE_EDITED` | true = keep the history |
| `void (Ctx&, const ev::MessageCreate&)`, ... | `EVENTS` | one handler per kind, batched; `ev::Overflow` reports losses |
| `void (Ctx&, const Message&, Decoration&)` | `DECORATE_MESSAGE` | filter `MC_FILTER_*` |
| `void (Ctx&, const User&, Decoration&)` | `DECORATE_USER` | member rows, authors, profiles; filter `1u << MC_USER_*` |
| `void (Ctx&, const Channel&, Decoration&)` | `DECORATE_CHANNEL` | channel and DM rows |
| `void (Ctx&, const Guild&, Decoration&)` | `DECORATE_GUILD` | server icons |
| `void (Ctx&, const Header&, Decoration&)` | `DECORATE_HEADER` | channel and member-list headers |
| `void (Ctx&, const NodeAction&)` | `NODE_ACTION` | a decoration button or card button was clicked |
| `bool (Ctx&, const TypingNotice&)` | `PRE_TYPING` | true = don't send the typing notice |
| `void (Ctx&, const SettingChanged&)` | `SETTING_CHANGED` | |
| `void (Ctx&, const KeybindPressed&)` | `KEYBIND` | a `Keybind` setting was pressed |
| `void (Ctx&, const Timer&)` | `TIMER` | from `ctx.timer(id, ms, repeat)` |

Every C++ plugin also gets the `COMPLETION` hook; results of `ctx.http`,
`ctx.modal` and `ctx.banner` go to the handler passed as a template argument.

Hooks run on the UI thread under a time budget (10 ms; decorations 1 ms).
Repeated overruns turn the plugin off. Decorate hooks run in the host's
prepare pass and are cached by entity revision until `ctx.invalidate_*()`.

### Decorations

```cpp
static void decorate(Ctx& ctx, const Message& m, Decoration& d) {
    d.badge(badge("NEW", Tone::accent));
    d.accessory(link("Docs", "https://example.com"));
    d.card(card().title("Title").text("Plain text\nwith lines").button("Hide", m.id()));
}
```

`Decoration` has `hide()`, `collapse(label)`, `blur_media()`, `hide_media()`,
`prefix(text, tone)`, `suffix(text, tone)`, `replace_content(markdown)`, up to
4 `badge(node)`, 8 `accessory(node)` and one `card(card)`. Nodes are `label`,
`badge`, `icon`, `button(text, tag)` and `link(text, url)`. Colours come from
`Tone` (theme roles; `Tone::role` is the author's role colour for name
affixes). Button and card-button tags are 1..2^63-1 and reach `NODE_ACTION`.
`ctx.push_message/user/channel/guild/header()` set a decoration without a
pull hook.

## Commands, menus and buttons

```cpp
MC_COMMAND(shout, (Str text, Opt<i64> times), "Shout something", .args = {"What to shout"}) {
    return Reply::insert(text);          // none, send, insert, local or toast
}

MC_SUBMENU(tools, menu::link | menu::image, "Tools");
MC_MENU(copy_link, menu::link, "Copy link", .parent = tools, .visible = has_url) {
    ctx.copy(target.url());
}

MC_BUTTON(peek, surface::message_popover, "Peek", "lucide/eye", .visible = has_text) {
    ctx.toast(button.message().content());
}
```

- Command arguments are `Str`, `i64`, `bool`, `UserId`, `ChannelId`, or
  `Opt<...>` for optional ones (required first); their names are the
  parameter names. The composer autocompletes from the descriptor.
- Menu targets: `menu::user`, `message`, `channel`, `guild`, `link`, `image`,
  `emoji`. One submenu level; `.checked = fn` makes a checkbox, `.danger`
  a destructive item.
- Button surfaces: `surface::chat_bar`, `message_popover`, `account_panel`,
  `header`, `server_list_top`, `server_list_bottom`. `.toggle = true` shows
  the state `ctx.button<name>(ButtonState{...})` sets (active, disabled,
  hidden, badge count, tooltip, icon).

The bodies see `ctx` and, for menus, `target` (`MenuTarget`) or, for
buttons, `button` (`ButtonEvent`).

## Host calls (`Ctx`)

| Call | Notes |
|---|---|
| `toast(text, tone)`, `log(level, text)` | |
| `store.get/put/remove` | 256 keys, 64 KiB values, 1 MiB total; persisted |
| `settings()`, `set_setting(id, value)`, `state<T>()` | |
| `http.get<F>(url, tag, headers)`, `http.post<F>(url, body, tag, headers)` | F: `void (Ctx&, const HttpResult&, u64 tag)`; 30 per minute, 4 in flight, 1 MiB answers |
| `banner<F>(Banner)`, `banner(Banner)`, `clear_banner()` | F: `void (Ctx&, const BannerResult&, u64)` |
| `modal<F>(Modal, tag)` | a confirmation or a small form; F: `void (Ctx&, const ModalResult&, u64)` |
| `open_url(url)`, `copy(text)`, `open_channel(id)`, `open_profile(user, guild)` | gestures |
| `local_message(text, channel)`, `remove_local_message(channel, id)` | only you see it |
| `composer.text/insert/replace/focus/reply/edit` | |
| `send`, `edit_message`, `react`, `delete_message`, `mark_read`, `set_activity` | |
| `timer(id, ms, repeat)`, `cancel_timer(id)` | 16 timers, at least 100 ms |
| `read.user/channel/guild/permissions` | cache only; never fetches |
| `kept.info/text/restore` | messages kept by `MESSAGE_DELETED`/`EDITED` |
| `invalidate_message/user/channel/guild`, `invalidate_headers`, `invalidate_buttons` | |

Helpers: `Text<N>` (fixed-capacity text), `Str`, `StrList`, `IdList`,
`url::strip_params`, `url::encode`, `json::find`, `json::read_string`,
`json::skip`.

## Writing a plugin in C

Fill an `mc_descriptor` by hand: arrays of `mc_setting_def`, `mc_hook` (a
kind and the matching `mc_hook_fn` member), `mc_command_def`, `mc_menu_def`
and `mc_button_def`, and call the host through `ctx->api`.
`tests/app/plugins/plugin_abi_c.c` is a complete example. A C plugin that
uses `http`, `modal` or `banner` needs its own `MC_HOOK_COMPLETION` hook.

## Versioning

The host supports exactly one ABI. Within ABI 6, `mc_api` and the structs the
host writes only grow by appending; each release that adds entries bumps
`MC_API_LEVEL`. A plugin's `api_level` is the level it needs; an older host
refuses it. `MC_API_HAS(ctx, field)` tests for newer optional entries.
