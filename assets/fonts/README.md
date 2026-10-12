# Profile display fonts

Fonts for profile typography. Each comes from an independently licensed upstream
release (not a Discord installation or CDN bundle) and is registered lazily with
MaiCord's native font engine when a visible name needs that family. Interface text
uses the system font. No font parsing or conversion library is shipped.

| Wire ID | File | Family / style | Bytes | License |
| --- | --- | --- | ---: | --- |
| 1 | [Bangers.ttf](Bangers.ttf) | Bangers | 93,148 | [OFL-Bangers.txt](OFL-Bangers.txt) |
| 2 | [BioRhyme.ttf](BioRhyme.ttf) | MaiCord BioRhyme / Regular | 75,888 | [OFL-BioRhyme.txt](OFL-BioRhyme.txt) |
| 3 | [CherryBomb.ttf](CherryBomb.ttf) | CherryBomb | 214,940 | [OFL-CherryBomb.txt](OFL-CherryBomb.txt) |
| 4 | [Chicle.ttf](Chicle.ttf) | Chicle | 41,032 | [OFL-Chicle.txt](OFL-Chicle.txt) |
| 5 | [Compagnon.otf](Compagnon.otf) | Compagnon | 54,376 | [OFL-Compagnon.txt](OFL-Compagnon.txt) |
| 6 | [MuseoModerno.ttf](MuseoModerno.ttf) | MaiCord MuseoModerno / Regular | 89,968 | [OFL-MuseoModerno.txt](OFL-MuseoModerno.txt) |
| 7 | [NeoCastel.otf](NeoCastel.otf) | MaiCord Medieval / Regular | 17,148 | [OFL-NeoCastel.txt](OFL-NeoCastel.txt) |
| 8 | [Pixelify.ttf](Pixelify.ttf) | MaiCord Pixelify / Regular | 51,912 | [OFL-Pixelify.txt](OFL-Pixelify.txt) |
| 9 | [Ribes.otf](Ribes.otf) | Ribes | 36,208 | [OFL-Ribes.txt](OFL-Ribes.txt) |
| 10 | [Sinistre.otf](Sinistre.otf) | Sinistre | 120,700 | [OFL-Sinistre.txt](OFL-Sinistre.txt) |
| 12 | [ZillaSlab.ttf](ZillaSlab.ttf) | ZillaSlab | 270,104 | [OFL-ZillaSlab.txt](OFL-ZillaSlab.txt) |
| 13 | [Playpen.ttf](Playpen.ttf) | MaiCord Playpen / Regular | 748,844 | [OFL-Playpen.txt](OFL-Playpen.txt) |
| 14 | [Orbitron.ttf](Orbitron.ttf) | Orbitron / Bold (Mainframe) | 37,648 | [OFL-Orbitron.txt](OFL-Orbitron.txt) |
| 15 | [NewRocker.ttf](NewRocker.ttf) | NewRocker | 173,224 | [OFL-NewRocker.txt](OFL-NewRocker.txt) |
| 16 | [Kalam.ttf](Kalam.ttf) | Kalam | 427,360 | [OFL-Kalam.txt](OFL-Kalam.txt) |

ID 11 selects the normal system face. The fifteen files total **2,452,500 bytes**.
Each ships with its complete SIL Open Font License 1.1 text (Neo Castel's is the
French translation); original notices stay in the files. Sources, retrieval dates,
transformations and final SHA-256 digests are in [sources.json](sources.json).

- **BioRhyme, MuseoModerno, Pixelify, Playpen:** instanced at weight 400 with all
  glyphs kept and the upstream defaults for other axes (fontTools 4.63.0, asset
  preparation only). Renamed `MaiCord BioRhyme`, `MaiCord MuseoModerno`,
  `MaiCord Pixelify` and `MaiCord Playpen` so the modified files do not claim the
  reserved upstream family names.
- **Neo Castel:** Max Lillo's free font ([author release](https://maxlilllo.gumroad.com/l/neo-castel));
  source and full license come from the `hunterirving/web_workshop` mirror linked in
  the manifest. Its WOFF2 was decoded to SFNT with fontTools and Brotli 1.2.0 outside
  the runtime; the outlines are unchanged. Family, style and unique-name metadata
  are normalized to `MaiCord Medieval Regular` (Regular style flags, weight 400)
  because the source's smiley subfamily metadata broke DirectWrite family matching.
- **Orbitron Bold and the other static faces:** unmodified. Compagnon is from
  Velvetyne; Ribes and Sinistre are from Collletttivo; the rest are from the Google
  Fonts upstream repository.

CMake copies this directory beside the application and the native UI probe. Keep it
next to the executable and its matching account module.
