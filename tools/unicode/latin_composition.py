"""Pinned UCD archive hash and Windows-specific Latin composition pairs.

Shared by alphabetic_composition.py (which generates
src/text/generated/alphabetic_composition_data.inc) and the CJK run oracle.
"""
PIN='2066d1909b2ea93916ce092da1c0ee4808ea3ef8407c94b4f14f5b7eb263d28e'
# Additional ordered pairs observed through first-party cmap-only fonts on the
# supported Windows reference. They are not Unicode canonical decomposition
# records; keep their provenance distinct from the UCD-derived entries.
WINDOWS_PAIRS=[(0x14c,0x328,0x1ec),(0x14d,0x328,0x1ed),(0x114,0x327,0x1e1c),(0x115,0x327,0x1e1d),
 (0xc2,0x323,0x1eac),(0x102,0x323,0x1eb6),(0xe2,0x323,0x1ead),(0x103,0x323,0x1eb7),
 (0xca,0x323,0x1ec6),(0xea,0x323,0x1ec7),(0xd4,0x323,0x1ed8),(0xf4,0x323,0x1ed9)]
