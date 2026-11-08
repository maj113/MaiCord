# Desktop integration on Linux

`linux/` holds the Linux implementations of the interfaces in `include/maicord/platform/desktop`. Notifications, the tray, single
instance, the file chooser and startup/scheme registration are over D-Bus: see `../dbus/README.md`. Game detection reads
`/proc/*/comm`.

## Spell checker (`linux/spell_checker.cpp`)

Hunspell (`libhunspell-1.7.so.0`), else Enchant 2 (`libenchant-2.so.2`), both `dlopen`ed, for the locale of `LC_ALL`/`LC_MESSAGES`/
`LANG` (else `en-US`; `de_AT.UTF-8` is "de-AT": that dictionary, else the base language's, else en-US, the choice of
`spell_checker_choose_language`). Hunspell dictionaries come from `$DICPATH`, the XDG data directories, `/usr/share/hunspell`,
`/usr/share/myspell` and the NixOS profiles (UTF-8 only). `MAICORD_SPELLCHECK_BACKEND=hunspell|enchant` forces one; a missing
library is "unavailable". "Add" is Enchant's personal list, or for Hunspell one word per line in
`<data directory>/spellcheck/<tag>.txt`; "Ignore" lasts for the checker.

Tests use `tests/fixtures/hunspell` and, with `MAICORD_TEST_SPELL_SET=ubuntu` (the registry sets it), the packages `hunspell-en-us`,
`hunspell-de-de` and `libenchant-2-2`.

## Security keys (`linux/fido2.cpp`, `linux/webauthn_ceremony.cpp`)

WebAuthn is the security-key ceremony over libfido2 (`libfido2.so.1`, `dlopen`ed with the table of
`include/maicord/platform/desktop/fido2/abi.hpp`, checked by `tools/check_fido2_abi.py`). The library is optional: without it
`webauthn_platform_available()` is false and the sign-in says so at once, choosing another second factor first when the account has
one. No software authenticator exists and the WSL kernel has no `/dev/uhid` or `hidraw`, so real-key behaviour (hidraw access, PIN
and user-verification flows, `ACTION_TIMEOUT` pacing, platform authenticators) is not validated on hardware.

- `application_login_run_webauthn` is split in `auth.cpp` into `webauthn_begin` (guards, decoded challenge, credential buffer,
  captured screen) and `webauthn_finish` (screen checked again, credential to the server). On Windows the call between them is the
  blocking `windows_webauthn_authenticate`; on Linux it is `webauthn_ceremony_start`, and the end of the ceremony arrives as a wake
  (`wake_begin_webauthn`, `b` 0 begin, 1 PIN, 2 done, into `application_login_webauthn_progress`).
- The ceremony is one worker thread (a static `Ceremony`): it hashes the client data (SHA-256 of `webauthn_mfa_write_client_data`),
  then `fido2::run` lists keys with `fido_dev_info_manifest` (hidraw; it looks again every 250 ms while none is plugged in, until the
  request's timeout, 120 s when it has none), opens each and calls `fido_dev_get_assert` with the relying party, the client data
  hash, every allowed credential and user verification as asked (required: true, discouraged: false, preferred: left to the key).
- Outcomes (`fido2::Outcome`): a PIN request (`FIDO_ERR_PIN_REQUIRED`, or `FIDO_ERR_PIN_INVALID` with the attempts
  `fido_dev_get_retry_count` reports) is asked through the hook, which posts a wake with `b = kWebAuthnWakePin` and waits for
  `webauthn_ceremony_submit_pin`; `FIDO_ERR_ACTION_TIMEOUT` repeats until the deadline; no credential on any key is `not_registered`;
  keys that cannot be opened (the udev rules for hidraw, from `libfido2` or `libu2f-udev`) are `cannot_open`.
- The credential id, authenticator data (`fido_assert_authdata_ptr`, its CBOR byte-string header stripped: the raw accessor is
  libfido2 1.14, newer than Ubuntu 22.04's 1.10 and Debian 12's 1.12),
  signature and user handle go into `webauthn_mfa_write_assertion` (attachment `cross-platform`), the same JSON as Windows.
- Screens: `LoginSecurityKey` (Linux-only, with `LoginRuntime::security_key` and `security_key_pin`): the wait for the touch is the
  `submitting` screen ("Touch your security key."; Back and Esc work through `invalidate_auth()`, which cancels the ceremony with
  `fido_dev_cancel`, as does a switch of method or account); the PIN is typed in the MFA code field on the `mfa_required` screen,
  masked, labelled "SECURITY KEY PIN", sent with "Verify" at four characters or more (a wrong one asks again, with the attempts
  left). A late answer from a screen that is gone is joined and dropped.
- Tests: `maicord_linux_fido2` (a fake libfido2 table: success, PIN required, wrong PIN, blocked PIN, no device, no credential,
  never touched, cannot open, cancel while waiting, through `fido2::run` and the worker-thread ceremony),
  `maicord_linux_fido2_no_library`, `maicord_linux_webauthn_flow` (the sign-in over a fake ceremony on the headless window) and
  `maicord_linux_webauthn_text`.

## Launch line

`src/app/linux/entry.cpp` answers `--version` first (the build's number on standard output, exit 0; it needs no display and no
io_uring), and turns every other `argv` into the one command line `desktop_launch_parse` reads (`launch_line.hpp`, tested by
`maicord_linux_launch_line`): `--startup`, `--url -- <link>`, a bare `discord://` argument and single-instance forwarding are the
Windows startup's.
