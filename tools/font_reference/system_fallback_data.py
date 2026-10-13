"""How src/ui/generated/system_fallback_data.inc was observed; not rerunnable.

The table is the en-US system font fallback policy for the default UI text
system (Segoe UI): 503 ordered scalar intervals, each naming an ordered list of
at most five target families, resolved by family name at run time.

Observation, public API only: IDWriteFontFallback::MapCharacters on the system
fallback (IDWriteFactory2::GetSystemFontFallback) through
font_fallback_reference.cpp, Windows 10.0.29648 with its stock fonts:
- every valid scalar at weights 400 and 600, upright and italic, recording the
  selected family and mapped length;
- joining controls, unsupported emoji in ordered pairs and all 260 variation
  selectors against each missing group, isolating routes from attachment;
- mixed strings at route boundaries and eight base-family configurations.

A constraint solver over the scalar winners, each family's cmap coverage and
the positive and negative range constraints ordered the families per interval
and merged adjacent intervals. Its input model is pinned by the SHA-256 in the
table header. Four further complete sweeps, each scalar followed by a joining
control, VS16 and a Latin, Greek or Han anchor, reproduced every query.
Regenerating means repeating the sweeps on the target Windows build and solving
again.
"""

if __name__ == '__main__':
    raise SystemExit(__doc__)
