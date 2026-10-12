# Security policy

## Reporting a vulnerability

Please report security problems privately through
[GitHub security advisories](https://github.com/maj113/MaiCord/security/advisories/new),
not as a public issue. Include the MaiCord version, what an attacker can do,
and steps to reproduce. You'll get a reply within a few days.

Examples of what to report:

- Ways to read or leak a saved sign-in, token or message content
- Bypassing the update signature check or installing an unsigned update
- Remote content (messages, embeds, images, videos) that crashes MaiCord or
  runs code
- Plugins gaining permissions the user didn't approve

## Supported versions

Only the latest release receives fixes. MaiCord updates itself, so most users
get fixes automatically.

## Out of scope

- Plugins you installed yourself: plugins are native code with full access to
  your computer, by design.
- Discord's own servers and services: report those to
  [Discord](https://discord.com/security).
