#pragma once


// Opening a link or a file URI in the user's browser or viewer: the OpenURI portal
// (org.freedesktop.portal.OpenURI.OpenURI on the session bus), and only when the portal is not there or refuses the call,
// the `xdg-open` command (started with posix_spawnp, found through PATH). UI thread.
//
// The spawned command is not waited for. Child processes are not reaped one by one: the first spawn sets SIGCHLD to be
// ignored (SA_NOCLDWAIT semantics), which is why nothing else in the process may wait for children.

namespace maicord::dbus {

// A URI is accepted when it has a scheme ("https:", "mailto:", "file:"), no control characters, and fits in 4096 bytes;
// a command line option (a leading '-') never is.
bool open_uri_valid(const char* uri) noexcept;

// Asks the desktop to open `uri`. `activation_token` (from xdg-activation or a notification's ActivationToken) lets the
// opened program take focus; null for none. True when the request was sent or the fallback started; the portal's refusal
// after that is handled by the fallback.
bool open_uri(const char* uri, const char* activation_token) noexcept;

} // namespace maicord::dbus
