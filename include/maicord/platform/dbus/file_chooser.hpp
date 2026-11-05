#pragma once

#include "maicord/core/base.hpp"

// Open and save dialogs through the org.freedesktop.portal.FileChooser portal (OpenFile and SaveFile on the session bus),
// with the shape of the Windows native_file_picker (platform/desktop/file_picker.hpp): the caller starts a dialog, polls
// its state from the UI loop, reads the chosen paths, may cancel, and releases it. The portal draws the dialog (native in
// GNOME and KDE, sandboxed-safe in Flatpak); an existing destination has already passed its overwrite prompt.
//
// Paths are UTF-8 absolute file paths (file:// URIs decoded; a portal that returns a document-portal path in a sandbox
// returns that path). The result is all-or-nothing: more than kFileChooserMaximumFiles ends as `capacity`.
//
// UI thread; the state changes inside the session bus processing (session.hpp).

namespace maicord::dbus {

inline constexpr u32 kFileChooserMaximumFiles = 64;
inline constexpr u32 kFileChooserPathBytes = 1024;

struct FileFilter {
    const char* name;                  // "Images"
    const char* const* patterns;       // globs: "*.png", "*.PNG"
    u32 pattern_count;
};

struct FileChooserOptions {
    const char* title;           // null for the portal's default wording
    const char* parent;          // the portal's window identifier ("wayland:<handle>", "x11:<xid>"); null for none
    const FileFilter* filters;   // the first is selected
    u32 filter_count;
    bool multiple;               // open: several files
    bool directory;              // open: choose folders
    const char* accept_label;    // the confirm button's text; null for the default
    const char* current_name;    // save: the suggested file name
    const char* current_folder;  // the folder to start in (an absolute path); null for the portal's choice
    // Called once, on the UI thread inside the session bus processing, when the state leaves `pending` through the
    // portal's answer or a failed call; not for file_chooser_cancel or file_chooser_release, which the caller makes
    // itself. May be null.
    void (*settled)(void* context) noexcept;
    void* settled_context;
};

enum class FileChooserState : u8 {
    pending,    // the dialog is up
    done,       // paths are available
    cancelled,  // the user closed it, or file_chooser_cancel
    capacity,   // more files than kFileChooserMaximumFiles, or a path too long: nothing is returned
    failed,     // no portal, a refusal, or an answer that was not file URIs
};

struct FileChooser;

// Starts a dialog; null when the request could not be made at all (no session bus). A missing portal shows up as `failed`.
FileChooser* file_chooser_open(const FileChooserOptions& options) noexcept;
FileChooser* file_chooser_save(const FileChooserOptions& options) noexcept;
FileChooserState file_chooser_state(const FileChooser* chooser) noexcept;
u32 file_chooser_count(const FileChooser* chooser) noexcept;
// The index-th chosen path, valid until release; null out of range.
const char* file_chooser_path(const FileChooser* chooser, u32 index) noexcept;
// Closes the dialog (Request.Close); the state becomes `cancelled` when it was pending.
void file_chooser_cancel(FileChooser* chooser) noexcept;
// Frees the chooser; a dialog still up is closed. Safe at any time.
void file_chooser_release(FileChooser* chooser) noexcept;

// The file:// URI as a path (percent escapes decoded; only an empty or `localhost` host); false for anything else or a
// path that does not fit.
bool file_uri_to_path(const char* uri, char* path, u32 capacity) noexcept;

} // namespace maicord::dbus
