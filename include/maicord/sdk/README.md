# MaiCord plugin SDK (ABI 6)

A MaiCord plugin is a native x64 DLL with no entry point whose only export is
one `const` descriptor, `maicord_plugin_v6`. The descriptor declares
everything the plugin does: metadata, permissions, settings, state, hooks,
slash commands, context-menu items, buttons and the HTTPS hosts it may reach.
Loading a plugin runs none of its code; the host checks the PE and the
descriptor first.

- `maicord_plugin.h` is the C ABI (C11 or C++). It's the reference for every
  struct, constant and host function.
- `plugin.hpp` is the C++23 authoring layer: one `.cpp` file of declarations
  becomes the descriptor at compile time. No STL, CRT, exceptions or RTTI.
- `background.hlsli` is the interface of background effects, which are shaders
  and not plugins; see `samples/effects/README.md`.
- `samples/plugins/` has working plugins: `clear-urls` (hooks),
  `translate` (network, buttons, cards, a command, state), `show-me-your-name`
  (user decorations), `reverse-image-search` (a submenu) and `silent-toggle`
  (a toggle button and a command).
- `tools/maicord-plugin` is the command line that writes, builds, loads,
  checks and packages plugins and effects.

Plugins are trusted native code: the host attributes crashes and hangs and
turns misbehaving plugins off, but it isn't a sandbox.

## Quick start

You need Windows 10 or 11 x64 and Python 3.9 or newer. You don't need Visual
Studio or the Windows SDK: the tool compiles with MaiCord's pinned clang-cl
and lld-link (downloaded once into `%LOCALAPPDATA%\MaiCord-toolchain`, checked
against the SHA-256 in `tools/llvm_toolchain.json`, or taken from `PATH`) and
against the headers in this folder only.

```powershell
# in a MaiCord checkout (or an SDK folder); put tools\maicord-plugin on PATH
maicord-plugin new hello        # hello\plugin.cpp, README.md, .gitignore, .clangd
cd hello
maicord-plugin build            # build\hello.dll, checked
maicord-plugin dev --watch      # copy it into MaiCord, again on every save
```

1. In MaiCord open **Settings > Advanced** and turn on **Developer Mode**.
2. Open **Settings > Plugins > Developer**. The first build of a plugin asks
   for your consent as an *unreviewed plugin*; accept, and `hello` runs: its
   `/hello` command and its **Sign my messages** setting are there.
3. Edit `plugin.cpp` and save. `dev --watch` rebuilds and copies the DLL; MaiCord
   notices within about a second, turns the plugin off, unloads it, loads the
   new build and turns it on again, **without asking again** while the
   permissions and hosts stay within what you allowed. Its state, settings and
   store carry over (state when its layout is unchanged, see "State").
4. The **Console** card of the Developer page shows what the plugin logs with
   `ctx.log`, a line for every reload ("Reloaded in 12 ms. State kept.") and
   every time budget it overran, with its strike count.

`maicord-plugin new` picks a random 64-bit **owner id** for the plugin. It is
the plugin's identity (its settings and store live under it): keep it for the
life of the plugin and never copy another plugin's.

The commands:

| Command | What it does |
|---|---|
| `new <name>` | Creates the project: a ClearURLs-sized `plugin.cpp` (a setting, a hook, a command, state), `README.md`, `.gitignore`, and a `.clangd` so editors find these headers |
| `build` | Compiles and links `plugin.cpp` into `build\<folder>.dll` with the flags of the catalog's CI (so the DLL is byte for byte what CI builds), then checks the PE and the descriptor |
| `check [dll]` | Runs those checks again, offline: no entry point, no TLS, one export, sections, descriptor strings, limits, hooks and commands in code |
| `dev [--watch]` | Builds if the DLL is stale, then copies it atomically into the dev folder. `--watch` rebuilds when a source changes |
| `pack` | Checks, then writes `dist\<slug>\` (`plugin.cpp`, `catalog.json`, `details.md`) for a pull request to the catalog; see "Publishing" |
| `effect new / build / preview` | The same for background effects |

Useful options: `--dir` (project folder), `--sdk` (a folder with
`include\maicord\sdk`), `--llvm-bin` (a folder with `clang-cl.exe` and
`lld-link.exe`), `--dev-folder`, `-v` (print the compiler commands). The dev
folder is `%LOCALAPPDATA%\MaiCord\plugins\dev`, or `%MAICORD_PLUGIN_DEV%` for
both MaiCord and the tool; effects use `...\effects\dev` and
`%MAICORD_EFFECT_DEV%`. The tool's own test is
`python -B tools/maicord-plugin/test_maicord_plugin.py`.

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
  (`maicord-plugin build` does.)
- Every entry point gets a `Ctx&` that is valid until it returns. Strings the
  host passes in are valid for the call; copy what you keep.
- A mistake the compiler can see is a compile error with a sentence: a missing
  permission (`ctx.copy needs .permissions = MC_PERM_CLIPBOARD`), a second
  hook of one kind, a command with the wrong parameters.

By hand, with clang-cl and lld-link (`<include>` holds `maicord/sdk/`; these
are the flags `maicord-plugin build` uses):

```powershell
clang-cl /nologo /c /std:c++latest /clang:-nostdinc /O2 /W4 /WX /utf-8 /Zl /GS- /GR- /permissive- `
  /clang:-fno-asynchronous-unwind-tables /clang:-fno-unwind-tables /DMC_RUNTIME `
  /I<include> /I<include>\maicord\sys\libc plugin.cpp
lld-link /nologo /DLL /NOENTRY /NODEFAULTLIB /SUBSYSTEM:WINDOWS,6.02 /DYNAMICBASE /HIGHENTROPYVA `
  /NXCOMPAT /CETCOMPAT /OPT:REF /OPT:ICF=8 /INCREMENTAL:NO /ALLOWBIND:NO /MERGE:.pdata=.rdata `
  /DEBUG:NONE /Brepro plugin.obj /OUT:my-plugin.dll
```

With CMake, `cmake/test_support.cmake` has `maicord_plugin(<target> <source>)`,
which this repository uses for its samples and fixtures.

## Descriptor (`MC_PLUGIN`)

| Option | Meaning |
|---|---|
| `.owner` | Non-zero 64-bit id; names the plugin and its data |
| `.name`, `.version` | 1..64 and 1..32 bytes. The catalog wants `MAJOR.MINOR.PATCH` |
| `.summary`, `.author`, `.homepage` | Optional; the homepage is `https://` |
| `.permissions` | `MC_PERM_*` bits the API calls need (below) |
| `.network` | Up to 8 HTTPS hosts: `"api.example.com"` or `"*.example.com"` |
| `.api_level` | Defaults to the header's `MC_API_LEVEL` |
| `.start`, `.stop` | `void (Ctx&)`; optional; `start` runs with the context flags `first_start()` or `reloaded()` |

## Permissions

The host adds the permissions hooks, buttons and hosts imply, so declare only
what API calls need. The C++ layer makes a missing permission a compile error.
At the first enable (and again when an update adds one) the user sees one line
per permission, grouped by risk, and the hosts:

| Permission | Implied by / needed for | What the consent says |
|---|---|---|
| `MC_PERM_MESSAGE_SEND` | `PRE_SEND`, `PRE_EDIT`, `PRE_TYPING` hooks | Change the messages you send or edit before they go out |
| `MC_PERM_READ_MESSAGES` | `MESSAGE_CLICK`, `DECORATE_MESSAGE`, message events, popover buttons | Read your messages and who you talk to |
| `MC_PERM_MESSAGE_HISTORY` | `MESSAGE_DELETED`, `MESSAGE_EDITED` | Keep deleted and edited messages visible while MaiCord is open |
| `MC_PERM_READ_ACTIVITY` | Presence, voice, server, member and friend events; a user's `presence` and `platforms` (else unknown and 0); `read.voice_channel` | See typing, presence, voice, server and friend activity |
| `MC_PERM_NETWORK` | A non-empty `.network`; `ctx.http` | Connect to the servers listed below |
| `MC_PERM_COMPOSER` | `ctx.composer.*` | Read and change what you are typing |
| `MC_PERM_ACT_MESSAGES` | `ctx.send`, `edit_message`, `react`, `delete_message`, `mark_read` | Send, edit and delete your messages, add reactions and mark channels read |
| `MC_PERM_ACT_PRESENCE` | `ctx.set_activity` | Set your activity and custom status |
| `MC_PERM_CLIPBOARD` | `ctx.copy` | Copy text to your clipboard |
| `MC_PERM_AUTOMATED_ACTIONS` | Writes outside a gesture (implies `ACT_MESSAGES`) | Act for you on its own, without you asking |
| `MC_PERM_PERSISTENT_LOG` | `ctx.kept.restore` (re-seeding kept messages from your own log) | Save deleted and edited messages on this computer |

The last two let a plugin act on its own or keep deleted messages across
restarts. The official catalog refuses them; sideloaded plugins can use them
after the user agrees to a separate consent line.

**Gestures.** Clicks on your buttons, menu items, card buttons and commands
are gestures (`ctx.gesture()`). A completion of a call started inside a
gesture keeps it for 5 seconds. Writes need a gesture unless the plugin has
`MC_PERM_AUTOMATED_ACTIONS`; `open_url`, `copy`, `open_*` and `modal` always
need one. Writes are rate-limited (`MC_E_RATE_LIMITED`).

**What a plugin never gets:** your token, cookies, raw REST access, other
plugins' data, or any host outside its declared `.network` (Discord's own
hosts are refused). Requests are HTTPS only, with no redirects off the list,
and the user can switch a host off on the plugin's settings page.

## Settings (`MC_SETTINGS`)

Each row is `(Type, field, "Label", default, options...)`. The host shows an
automatic settings page (Settings > Plugins > the plugin's cog) and writes the
values into `mc::Settings`, which handlers read with `ctx.settings().field`
(plain loads). `ctx.set_setting(Setting::field, value)` changes one from the
plugin; the new value shows from the next call. `SETTING_CHANGED` reports
changes the user makes.

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
Setting::choice, .visible_value = index`. The field name is the persisted key
(1..32 bytes of `[a-z0-9_]`): renaming a field resets that setting. Settings
persist across reloads and updates; a value the new schema no longer accepts
falls back to its default.

```cpp
MC_SETTINGS((Header, display, "Display"),
            (Choice, style, "Style", 0, .items = {"Plain", "Bold"}),
            (Slider, size, "Size", 14, .min = 10, .max = 24, .step = 2, .visible_if = Setting::style, .visible_value = 1),
            (Keybind, hotkey, "Shortcut", MC_KEY('K', MC_KEY_CTRL)),
            (TextList, words, "Words to watch", {"hello"}, .hint = "One per line"));

static void changed(Ctx& ctx, const SettingChanged& change) {
    if (change.index == static_cast<u32>(Setting::style))
        ctx.toast("Style changed");
}
static void pressed(Ctx& ctx, const KeybindPressed&) {
    ctx.toast("Hotkey");
}
```

## State and the store

`MC_STATE(Type, .version = n)` is host-owned memory your handlers reach with
`ctx.state<Type>()`. It must be trivially copyable and hold no pointers (not to
the DLL's code or data: the image is unloaded at a reload). It's zeroed at
the first start and survives a Developer Mode reload while its size,
alignment and `.version` stay the same: `start` then runs with
`ctx.reloaded()`; otherwise it runs with `ctx.first_start()` and the Console
says "State reset: its layout changed." Bump `.version` whenever the meaning
of the bytes changes. State doesn't persist across a restart of MaiCord.

`ctx.store` is a key-value store the host persists (256 keys, 64 KiB values,
1 MiB total; written shortly after the last change and on disable):

```cpp
u32 visits = 0;
ctx.store.get("visits", visits);          // false when the key has no value
ctx.store.put("visits", visits + 1);      // trivially copyable values, or Str text
ctx.store.remove("visits");
```

## Hooks (`MC_HOOKS`)

List plain functions; their parameter types pick the hook. Wrap one in
`mc::filter<fn, bits>` to set its filter. At most one hook per kind (events
merge: one handler per event type).

| Signature | Hook | Notes |
|---|---|---|
| `void (Ctx&, Draft&)` | `PRE_SEND` | `replace`, `cancel`, `confirm(question)`, `set_flag(MC_DRAFT_SILENT...)`; `reply()` and `reply_author()` name what it replies to |
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

**Time budgets.** Every call into your code is timed. A call that runs past
its budget is a *strike*: its output is ignored, and the third strike turns
the plugin off.

| Calls | Budget |
|---|---|
| Decorate hooks, button `.visible` | 1 ms |
| `PRE_SEND`, `PRE_EDIT`, `PRE_TYPING`, `MESSAGE_DELETED`, `MESSAGE_EDITED`, menu `.visible` | 10 ms |
| Events, timers, `SETTING_CHANGED`, completions | 10 ms |
| Commands, menu items, button and card clicks, `MESSAGE_CLICK`, keybinds | 50 ms |
| `start`, `stop` | 500 ms |

Hooks run on the UI thread, so never block: start an `http` request and
finish in its completion. Decorate hooks run in the host's prepare pass and
are cached by entity revision until `ctx.invalidate_*()`. A crash is
attributed to your plugin and hook, and it stays off at the next start.

### Examples

**`PRE_SEND` and `PRE_EDIT`** change or stop what you send (permission
`message_send`, implied):

```cpp
static void pre_send(Ctx& ctx, Draft& draft) {
    if (draft.text().find('@') < draft.text().size() && ctx.settings().ask)
        draft.confirm("Mention someone?");          // the user chooses; accepting sends the text as it is
    else if (draft.text() == Str("/shrug"))
        draft.replace("\xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF");
    draft.set_flag(MC_DRAFT_SILENT, true);            // @silent
}
static void pre_edit(Ctx&, EditDraft& draft) {
    if (draft.text().empty()) draft.cancel();
}
```

**`MESSAGE_CLICK`** returns true when it used the click (`read_messages`):

```cpp
static bool on_click(Ctx& ctx, const Message& message, const Click& click) {
    if (!click.double_click() || !message.own()) return false;
    ctx.composer.edit(message.channel(), message.id()); // needs .permissions = MC_PERM_COMPOSER
    return true;
}
MC_HOOKS(mc::filter<on_click, MC_CLICK_FILTER_DOUBLE>);
```

**`MESSAGE_DELETED` and `MESSAGE_EDITED`** return true to keep the message
visible this session (`message_history`); read what was kept with
`ctx.kept.info/text`:

```cpp
static bool deleted(Ctx&, const DeletedMessage& message) { return !message.bot(); }
static bool edited(Ctx&, const Message& before, const Message& after) { return before.own() == false; }
```

**`EVENTS`**: one function per kind; the host batches them, so a burst is
one call:

```cpp
static void on_create(Ctx& ctx, const ev::MessageCreate& e) { /* e.text() is the content: read, never log it */ }
static void on_presence(Ctx&, const ev::Presence& e) { /* e.name(), e.has(...) */ }
static void lost(Ctx& ctx, const ev::Overflow& e) { ctx.log(Level::warning, "dropped events"); }
```

**Decorations** (`DECORATE_MESSAGE`, `DECORATE_USER`, `DECORATE_CHANNEL`,
`DECORATE_GUILD`, `DECORATE_HEADER`):

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
pull hook. The host reads a decoration after the hook returns, so
`Decoration` copies the text you give it: building it from a local `Text<N>`
is fine (a C plugin must keep such text in static or state memory).

```cpp
static void on_node(Ctx& ctx, const NodeAction& action) {
    if (action.tag) ctx.invalidate_message(action.tag);   // the tag was the message id above
}
```

**`PRE_TYPING`** returns true to drop the typing notice:

```cpp
static bool quiet(Ctx& ctx, const TypingNotice&) { return ctx.settings().silent_typing; }
```

**`TIMER`**: `ctx.timer(id, ms, repeat)` (at least 100 ms, 16 at a time):

```cpp
static void start(Ctx& ctx) { ctx.timer(1, 60'000, true); }
static void tick(Ctx& ctx, const Timer& timer) { if (timer.id == 1) ctx.toast("A minute passed"); }
```

**`COMPLETION`** results go to the handler you name when you start the call:

```cpp
static void answered(Ctx& ctx, const HttpResult& r, u64 tag) {
    if (!r.ok()) { ctx.toast("Request failed", Tone::critical); return; }
    Text<MC_CARD_TEXT_MAX> text;
    if (json::find(r.body(), "text", text)) ctx.composer.insert(text);
}
// somewhere in a command or click:
ctx.http.get<answered>("https://api.example.com/v1/thing?q=1", /*tag*/ 7);
```

### Commands, menus and buttons

```cpp
MC_COMMAND(shout, (Str text, Opt<i64> times), "Shout something", .args = {"What to shout"}) {
    Text<MC_REPLY_TEXT_MAX> out;
    for (i64 i = 0; i < (times ? *times : 1) && i < 3; ++i)
        out.append(text);
    return Reply::insert(out);           // none, send, insert, local or toast
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
  parameter names. The composer autocompletes from the descriptor. A command
  replies with `Reply::none/send/insert/local/toast`; the text is copied when
  the `Reply` is made.
- Menu targets: `menu::user`, `message`, `channel`, `guild`, `link`, `image`,
  `emoji`. One submenu level; `.checked = fn` makes a checkbox, `.danger`
  a destructive item. A `.visible` function answers within 10 ms.
- Button surfaces: `surface::chat_bar`, `message_popover`, `account_panel`,
  `header`, `server_list_top`, `server_list_bottom`. `.toggle = true` shows
  the state `ctx.button<name>(ButtonState{...})` sets (active, disabled,
  hidden, badge count, tooltip, icon).

The bodies see `ctx` and, for menus, `target` (`MenuTarget`) or, for
buttons, `button` (`ButtonEvent`).

## Host calls (`Ctx`)

| Call | Notes |
|---|---|
| `toast(text, tone)`, `log(level, text)` | `log` goes to the Developer console (Developer Mode only): never put message content, tokens or URLs in it |
| `store.get/put/remove` | 256 keys, 64 KiB values, 1 MiB total; persisted |
| `settings()`, `set_setting(id, value)`, `state<T>()` | |
| `http.get<F>(url, tag, headers)`, `http.post<F>(url, body, tag, headers)` | F: `void (Ctx&, const HttpResult&, u64 tag)`; 30 per minute, 4 in flight, 1 MiB answers |
| `banner<F>(Banner)`, `banner(Banner)`, `clear_banner()` | F: `void (Ctx&, const BannerResult&, u64)` |
| `modal<F>(Modal, tag)` | a confirmation or a small form; F: `void (Ctx&, const ModalResult&, u64)`. For long read-only text use a form with `MC_MODAL_MULTILINE` and `input_initial` (up to 2,000 bytes): the box scrolls and can be selected |
| `open_url(url)`, `copy(text)`, `open_channel(id)`, `open_profile(user, guild)` | gestures |
| `local_message(text, channel)`, `remove_local_message(channel, id)` | only you see it |
| `composer.text/insert/replace/focus/reply/clear_reply/edit` | `composer` permission |
| `send`, `edit_message`, `react`, `delete_message`, `mark_read`, `set_activity` | gestures, or `automated_actions`; `set_activity(nullptr)` is never limited, and the host clears your activity when the plugin stops |
| `timer(id, ms, repeat)`, `cancel_timer(id)` | 16 timers, at least 100 ms |
| `now_ms()`, `local_offset(unix_seconds)` | the wall clock (Unix ms) and the user's UTC offset in seconds at that instant |
| `read.user/channel/guild/permissions` | cache only; never fetches. `read.guild` also counts boosts, channels, roles and emoji |
| `read.voice_channel(user, guild)` | the voice channel the user is in, or 0; needs `read_activity` |
| `read.open_channel()` | the open channel, the composer's reply target and the message being edited |
| `read.messages(channel, before, out)`, `read.shared_group_dms(user, out)` | the open channel's loaded messages (ids, authors, flags; no text), newest first; group DMs you share with a user. Both need `read_messages` declared |
| `kept.info/text/restore` | messages kept by `MESSAGE_DELETED`/`EDITED` |
| `invalidate_message/user/channel/guild`, `invalidate_headers`, `invalidate_buttons` | |

```cpp
// A confirmation, then an action when the user accepts.
static void confirmed(Ctx& ctx, const ModalResult& r, u64 channel) {
    if (r.accepted()) ctx.mark_read(MC_READ_CHANNEL, channel);   // needs MC_PERM_ACT_MESSAGES
}
MC_BUTTON(read_it, surface::header, "Mark read", "lucide/check") {
    Modal m{};
    m.title = Str("Mark this channel read?");
    m.accept = Str("Mark read");
    ctx.modal<confirmed>(m, button.channel());
}

// A banner with a button; opened is void (Ctx&, const BannerResult&, u64 tag).
Banner b{};
b.title = Str("Update available");
b.action = Str("Open");
ctx.banner<opened>(b);

// Your own activity.
Activity a{};
a.type = MC_ACTIVITY_PLAYING;
a.name = Str("Something");
ctx.set_activity(&a);                 // needs MC_PERM_ACT_PRESENCE

// Cache reads.
if (auto user = ctx.read.user(id)) ctx.toast(user->name());
```

Helpers: `Text<N>` (fixed-capacity text), `Str`, `StrList`, `IdList`,
`url::strip_params`, `url::encode`, `json::find`, `json::read_string`,
`json::skip`.

## Developing

- **Hot reload.** With Developer Mode on, MaiCord watches the dev folder.
  When a DLL there changes (it waits 300 ms after the last write), it copies
  it into its own `plugins` folder (so your build is never locked), turns the
  running plugin off (`stop` runs), unloads it, loads the new build and turns
  it on. Nothing is asked when the rebuild asks for no permission or host the
  plugin didn't already have; one that does gets the consent again. A build
  that fails the checks leaves the running one alone and prints why on the
  Console ("Not reloaded: The DLL has an entry point; link with /NOENTRY.").
  The same holds for effects: a rebuilt `.mcfx` in `effects\dev` is listed
  again, keeps its place as the background and keeps your knob values.
- **Console.** Settings > Plugins > Developer shows the newest lines: your
  `ctx.log` lines (named after the plugin; a level maps to a colour), the
  host's own notes (reloads, refused builds, declined consents) and every
  time-budget strike. It is in memory only, off with Developer Mode, and the
  host's lines never include message content. **Reload** scans the
  folder again and checks every build in it; **Clear** empties the console.
- **Sideloaded plugins** are marked everywhere, never update by themselves
  and only run while Developer Mode is on. A plugin that is also in the
  catalog (same owner id) can't be sideloaded.

Test a plugin by pressing its buttons, but also by making it fail: send
the empty string, a 4,000-byte message, an edit of someone else's message,
and a message while the network is down.

## Publishing to the catalog

The official catalog is the repository
[maj113/MaiCord-plugins](https://github.com/maj113/MaiCord-plugins): sources
only, never prebuilt DLLs. Its CI builds every plugin from its source with the
same pinned clang-cl and the SDK headers of MaiCord's `master`, so a plugin
that builds with `maicord-plugin build` builds there. A plugin is one folder:

```
plugins/<slug>/
    plugin.cpp       the only source file
    catalog.json     what the catalog shows
    details.md       the plugin's page
```

`maicord-plugin pack --author "Your Name" --github you --license MPL-2.0`
checks the DLL and writes that folder (as `dist\<slug>\`) from your project and
descriptor. `catalog.json` holds `name`, `summary`, `authors` (1 to 4 of
`{name, github?}`), `license` (an SPDX id that allows redistribution and
modification), `categories` (up to 3 of `chat`, `appearance`, `notifications`,
`privacy`, `utility`, `voice`, `accessibility`, `moderation`, `developer`,
`fun`), optional `homepage`, `derived_from` and `changelog`, and `owner_id`
(the descriptor's `.owner` as `0x` and 16 hex digits; CI refuses a mismatch
and duplicates). The version, API level, permissions and network hosts come
from the built DLL, so the index never disagrees with the binary.

`details.md` uses the Markdown subset the site and the client render:
headings, paragraphs, `-` lists, code, `**bold**`, `` `code` `` and https
links. No tables, images, HTML or numbered lists. Keep a `DETAILS.md` in your
project and `pack` copies it.

To publish:
1. Fork the catalog repository, copy `dist\<slug>\` to `plugins/<slug>/` and
   open a pull request. CI builds the plugin, checks the descriptor and
   prints the permissions and hosts the build reports.
2. A maintainer reviews the code and those permissions and hosts, and merges.
   CI on `main` builds again, renders the index, signs it and deploys it;
   clients see the plugin on their next catalog check.
3. For every later release, bump `.version` in `MC_PLUGIN` (CI fails when
   `plugin.cpp` changed and the version didn't; published versions are
   immutable and kept for rollback) and open another pull request.

The catalog refuses a plugin that could break Discord's Terms of Service:
automating the account (the `automated_actions` permission, or sending,
editing or reacting without the user's action), unlocking paid features, and
logging deleted messages (`persistent_log`); it also refuses hidden network
use, telemetry and obfuscated code. Such plugins can still be sideloaded by
users who turn on Developer Mode. A listed build can be revoked (its SHA-256
goes into the signed index and every client turns it off).

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
refuses it. `MC_API_HAS(ctx, field)` tests for newer optional entries. The
catalog hides builds whose level is above the user's MaiCord, and when the ABI
changes its CI rebuilds every listed plugin.
