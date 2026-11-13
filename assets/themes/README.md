# Theme format

A theme is a JSON file (up to 64 KiB) that starts from the Dark, Light or AMOLED base and can override any design token.
[`example.maicord-theme.json`](example.maicord-theme.json) lists every token with its dark default; it is generated from the
token tables (`tools/generate_theme_example.py`, checked on every build). An optional `background` member names an installed
effect and its knob values.

## Time of day (`variants`)

- `variants` holds up to four phases (`night`, `dawn`, `day`, `dusk`), each with a `base`, core `colors` and an optional
  `months` list of twelve colour sets (each turning toward the next month's from the 5th to the 30th, as the Silk Waves sky
  does).
- MaiCord holds each phase around its hours (night 23–01, dawn 05–07, day 11–13, dusk 17–19, the Silk Waves schedule), mixes
  the colours across the hours between, shows the nearer phase whole where the base changes (gliding there, at once with
  reduced motion), and lets the theme alone decide an effect's day and night.
- A phase starts from its base's built-in colours, so it is light or dark on its own: the root's `semantic` overrides don't
  apply to it, and its colours may name the root's `palette`. The root's typography, metrics, scale, motion, components and
  background apply to every phase.
- MaiCord before 0.6.4 skips `variants` as an unknown member and shows the root look.
- Such a theme can follow knobs of its effect (`background.follow`: a month knob and a phase knob with the phase each option
  holds), so a month or a day or night picked on the effect's knob page holds the theme there too while that effect is the
  background; see [`samples/effects/README.md`](../../samples/effects/README.md#time-of-day-and-appearance).

## Installing

Themes and effects come from the catalog, from a link (`https://…`), from a file or from a drop onto the install dialog.
Anything that is not from the catalog is shown as *not reviewed* with a live preview before it is installed, and never updates
on its own. `maicord://install/<theme|effect|plugin>/<slug>` links open the catalog entry in MaiCord. Registering MaiCord for
these links is optional: it is asked once, when the catalog is first opened, and only writes
`HKEY_CURRENT_USER\Software\Classes\maicord` on Windows, and a desktop entry and the default for the scheme in `mimeapps.list`
on Linux.
