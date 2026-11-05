# Time zones and dates

- `tzif.cpp` reads TZif files and POSIX rules as a pure function over bytes. `tests/platform/time/tzif_test.cpp` (`maicord_tzif`) runs
  on both systems against zic's output and Python's zoneinfo; `python3 tools/tzif_fixtures.py` (zic, Python 3.9, in WSL) regenerates
  `tests/generated/tzif_*.bin` and `tests/fixtures/zoneinfo`.
- Linux local time (`include/maicord/os/local_time.hpp`, `src/os/linux/local_time.cpp`): `$TZ` (a name, `:name`, a path or a POSIX
  rule), else `/etc/localtime`, else `/etc/timezone`, in `$TZDIR` or the usual zoneinfo directories (NixOS: `/etc/zoneinfo`). A zone that
  changed is read again within fifteen seconds; an unreadable one is UTC. The Linux log's stamps and `host_iana_timezone` use it.
  Windows keeps calling the Win32 zone functions.
- `include/maicord/os/date_format.hpp` is the one place that formats a date or a time as text: forwarders to `GetDateFormatEx`/
  `GetTimeFormatEx` on Windows; English names on Linux with the 12 or 24 hour cycle of `LC_ALL`/`LC_TIME`/`LANG`.
