"""Generate src/ui/text/generated/linux_fallback_data.inc, the Linux font fallback policy.

The table has the shape of the Windows one (system_fallback_data.inc: ordered scalar intervals, each naming an ordered
list of target families that the text catalog resolves by name), but it is written down instead of observed: Linux has
no system fallback service to observe, and fontconfig's own order is consulted at run time when libfontconfig is
present (native_system_fallback.inc asks FcFontSort first and appends this table's names after it). Without
fontconfig this table is the whole policy.

Policy, most specific first:
  * a script gets its Noto family (Noto Sans <Script>, then Noto Serif <Script> where Noto has one, then the
    distribution families commonly installed for it), then the generic tail: DejaVu Sans, Liberation Sans;
  * the Han, Hiragana, Katakana, Hangul and Bopomofo scripts and the CJK punctuation and compatibility blocks get the
    Noto Sans CJK families, ordered per language (six variants: unspecified, ja, ko, zh-cn, zh-tw, zh-hk) and the
    common distribution CJK families after them;
  * symbol, arrow, math, technical, box, geometric and dingbat blocks get the symbol families (DejaVu Sans, the Noto
    symbol, math and music families);
  * the emoji blocks get Noto Color Emoji, then the symbol families;
  * an unassigned scalar has no family.

Input: the pinned Unicode 17.0.0 UCD archive (the same one tools/unicode/generate.py pins), for Scripts.txt and
Blocks.txt. Run:

    python tools/font_reference/linux_fallback.py --ucd UCD.zip [--output src/ui/text/generated/linux_fallback_data.inc]

The output is deterministic; the header records the input hash and the script's own hash.
"""

import argparse
import hashlib
import pathlib
import sys
import zipfile

EXPECTED_UCD_SHA256 = '2066d1909b2ea93916ce092da1c0ee4808ea3ef8407c94b4f14f5b7eb263d28e'
LIMIT = 0x110000
MAX_FAMILIES = 16
LOCALES = ['default', 'ja', 'ko', 'zh-cn', 'zh-tw', 'zh-hk']

TAIL = ['DejaVu Sans', 'Liberation Sans']

# Scripts that have a Noto Serif family worth naming (Noto's serif subset).
NOTO_SERIF = {
    'Armenian', 'Bengali', 'Devanagari', 'Ethiopic', 'Georgian', 'Gujarati', 'Gurmukhi', 'Hebrew', 'Kannada', 'Khmer',
    'Lao', 'Malayalam', 'Myanmar', 'Sinhala', 'Tamil', 'Telugu', 'Thai', 'Tibetan', 'Tangut', 'Ahom', 'Dogra',
    'Khojki', 'Makasar', 'Nyiakeng_Puachue_Hmong', 'Old_Uyghur', 'Toto', 'Vithkuqi', 'Yezidi',
}

# Distribution families installed for a script besides Noto, in preference order.
EXTRA = {
    'Arabic': ['Noto Naskh Arabic', 'Noto Kufi Arabic', 'Amiri', 'Scheherazade New', 'Lateef'],
    'Bengali': ['Lohit Bengali', 'Mukti Narrow', 'Siyam Rupali'],
    'Devanagari': ['Lohit Devanagari', 'Sarai', 'Mukti Narrow'],
    'Gujarati': ['Lohit Gujarati', 'Samyak Gujarati'],
    'Gurmukhi': ['Lohit Gurmukhi'],
    'Kannada': ['Lohit Kannada', 'Tunga'],
    'Khmer': ['Khmer OS', 'Khmer OS System'],
    'Lao': ['Phetsarath OT', 'Saysettha OT'],
    'Malayalam': ['Lohit Malayalam', 'Meera', 'Rachana'],
    'Oriya': ['Lohit Odia', 'Lohit Oriya'],
    'Sinhala': ['Noto Sans Sinhala UI', 'Abyssinica SIL'],
    'Tamil': ['Lohit Tamil', 'Lohit Tamil Classical'],
    'Telugu': ['Lohit Telugu', 'Gautami'],
    'Thai': ['Loma', 'Garuda', 'Norasi', 'Waree', 'Tlwg Typo'],
    'Ethiopic': ['Abyssinica SIL', 'Noto Sans Ethiopic'],
    'Hebrew': ['Noto Sans Hebrew', 'FreeSans'],
    'Tibetan': ['Jomolhari', 'Tibetan Machine Uni'],
    'Myanmar': ['Padauk', 'Myanmar Text'],
    'Syriac': ['Estrangelo Edessa'],
    'Thaana': ['MV Boli'],
    'Armenian': ['FreeSans'],
    'Georgian': ['FreeSans'],
}

# Noto family names that are not "Noto Sans <Script>" spelled with spaces.
NOTO_NAMES = {
    'Nko': 'Noto Sans NKo',
    'Phags_Pa': 'Noto Sans PhagsPa',
    'Canadian_Aboriginal': 'Noto Sans Canadian Aboriginal',
    'SignWriting': 'Noto Sans SignWriting',
    'Meetei_Mayek': 'Noto Sans Meetei Mayek',
    'Oriya': 'Noto Sans Oriya',
    'Tangut': 'Noto Serif Tangut',
    'Khitan_Small_Script': 'Noto Serif Khitan Small Script',
    'Tai_Tham': 'Noto Sans Tai Tham',
    'Tai_Le': 'Noto Sans Tai Le',
    'Tai_Viet': 'Noto Sans Tai Viet',
    'New_Tai_Lue': 'Noto Sans New Tai Lue',
    'Yi': 'Noto Sans Yi',
    'Egyptian_Hieroglyphs': 'Noto Sans Egyptian Hieroglyphs',
    'Anatolian_Hieroglyphs': 'Noto Sans Anatolian Hieroglyphs',
    'Cuneiform': 'Noto Sans Cuneiform',
    'Nushu': 'Noto Serif Nushu',
    'Kawi': 'Noto Sans Kawi',
    'Nag_Mundari': 'Noto Sans Nag Mundari',
    'Mende_Kikakui': 'Noto Sans Mende Kikakui',
    'Pau_Cin_Hau': 'Noto Sans Pau Cin Hau',
    'Psalter_Pahlavi': 'Noto Sans Psalter Pahlavi',
    'Inscriptional_Pahlavi': 'Noto Sans Inscriptional Pahlavi',
    'Inscriptional_Parthian': 'Noto Sans Inscriptional Parthian',
    'Old_South_Arabian': 'Noto Sans Old South Arabian',
    'Old_North_Arabian': 'Noto Sans Old North Arabian',
    'Hanifi_Rohingya': 'Noto Sans Hanifi Rohingya',
    'Nyiakeng_Puachue_Hmong': 'Noto Serif Nyiakeng Puachue Hmong',
    'Old_Uyghur': 'Noto Serif Old Uyghur',
    'Zanabazar_Square': 'Noto Sans Zanabazar Square',
    'Sora_Sompeng': 'Noto Sans Sora Sompeng',
    'Caucasian_Albanian': 'Noto Sans Caucasian Albanian',
    'Bassa_Vah': 'Noto Sans Bassa Vah',
    'Warang_Citi': 'Noto Sans Warang Citi',
    'Masaram_Gondi': 'Noto Sans Masaram Gondi',
    'Gunjala_Gondi': 'Noto Sans Gunjala Gondi',
    'Syloti_Nagri': 'Noto Sans Syloti Nagri',
    'Kayah_Li': 'Noto Sans Kayah Li',
    'Ol_Chiki': 'Noto Sans Ol Chiki',
    'Old_Hungarian': 'Noto Sans Old Hungarian',
    'Old_Italic': 'Noto Sans Old Italic',
    'Old_Permic': 'Noto Sans Old Permic',
    'Old_Persian': 'Noto Sans Old Persian',
    'Old_Sogdian': 'Noto Sans Old Sogdian',
    'Old_Turkic': 'Noto Sans Old Turkic',
    'Linear_A': 'Noto Sans Linear A',
    'Linear_B': 'Noto Sans Linear B',
    'Imperial_Aramaic': 'Noto Sans Imperial Aramaic',
    'Pahawh_Hmong': 'Noto Sans Pahawh Hmong',
    'Cypro_Minoan': 'Noto Sans Cypro Minoan',
}

# CJK families by variant; the common distribution families follow the Noto ones.
CJK_NOTO = {
    'ja': ['Noto Sans CJK JP', 'Noto Sans JP'],
    'ko': ['Noto Sans CJK KR', 'Noto Sans KR'],
    'zh-cn': ['Noto Sans CJK SC', 'Noto Sans SC'],
    'zh-tw': ['Noto Sans CJK TC', 'Noto Sans TC'],
    'zh-hk': ['Noto Sans CJK HK', 'Noto Sans HK'],
}
CJK_OTHER = {
    'ja': ['IPAexGothic', 'IPAGothic', 'TakaoGothic', 'VL Gothic', 'Source Han Sans JP'],
    'ko': ['NanumGothic', 'UnDotum', 'Baekmuk Gulim', 'Source Han Sans KR'],
    'zh-cn': ['WenQuanYi Micro Hei', 'WenQuanYi Zen Hei', 'Source Han Sans SC', 'Droid Sans Fallback'],
    'zh-tw': ['AR PL UMing TW', 'WenQuanYi Zen Hei', 'Source Han Sans TC', 'Droid Sans Fallback'],
    'zh-hk': ['AR PL UMing HK', 'Source Han Sans HK', 'Droid Sans Fallback'],
}
# The order the variants are tried in after the preferred one, per locale.
CJK_ORDER = {
    'default': ['ja', 'zh-cn', 'zh-tw', 'ko', 'zh-hk'],
    'ja': ['ja', 'zh-cn', 'zh-tw', 'ko', 'zh-hk'],
    'ko': ['ko', 'ja', 'zh-cn', 'zh-tw', 'zh-hk'],
    'zh-cn': ['zh-cn', 'ja', 'zh-tw', 'ko', 'zh-hk'],
    'zh-tw': ['zh-tw', 'zh-hk', 'ja', 'zh-cn', 'ko'],
    'zh-hk': ['zh-hk', 'zh-tw', 'ja', 'zh-cn', 'ko'],
}

SYMBOLS = ['DejaVu Sans', 'Noto Sans Symbols', 'Noto Sans Symbols 2', 'Noto Sans Math', 'Noto Music', 'FreeSerif']
EMOJI = ['Noto Color Emoji', 'Noto Sans Symbols 2', 'Noto Sans Symbols', 'DejaVu Sans']
LATIN = ['Noto Sans', 'DejaVu Sans', 'Liberation Sans', 'Noto Serif', 'DejaVu Serif']
LAST_RESORT = ['Unifont', 'Noto Sans Symbols 2']
PAD_POOL = ['Unifont', 'Noto Sans Symbols 2', 'FreeSans', 'FreeSerif', 'Noto Sans Symbols']

# Common and Inherited scalars are placed by block. Block name -> group.
BLOCK_GROUPS = {
    'Basic Latin': 'latin', 'Latin-1 Supplement': 'latin', 'Latin Extended-A': 'latin', 'Latin Extended-B': 'latin',
    'IPA Extensions': 'latin', 'Spacing Modifier Letters': 'latin', 'Combining Diacritical Marks': 'latin',
    'General Punctuation': 'latin', 'Superscripts and Subscripts': 'latin', 'Currency Symbols': 'latin',
    'Combining Diacritical Marks for Symbols': 'symbols', 'Letterlike Symbols': 'symbols', 'Number Forms': 'latin',
    'Arrows': 'symbols', 'Mathematical Operators': 'math', 'Miscellaneous Technical': 'symbols',
    'Control Pictures': 'symbols', 'Optical Character Recognition': 'symbols', 'Enclosed Alphanumerics': 'symbols',
    'Box Drawing': 'symbols', 'Block Elements': 'symbols', 'Geometric Shapes': 'symbols',
    'Miscellaneous Symbols': 'symbols', 'Dingbats': 'symbols', 'Miscellaneous Mathematical Symbols-A': 'math',
    'Supplemental Arrows-A': 'symbols', 'Braille Patterns': 'symbols', 'Supplemental Arrows-B': 'symbols',
    'Miscellaneous Mathematical Symbols-B': 'math', 'Supplemental Mathematical Operators': 'math',
    'Miscellaneous Symbols and Arrows': 'symbols', 'Supplemental Punctuation': 'latin', 'Ideographic Description Characters': 'cjk',
    'Kangxi Radicals': 'cjk', 'CJK Radicals Supplement': 'cjk', 'CJK Symbols and Punctuation': 'cjk',
    'CJK Strokes': 'cjk', 'Enclosed CJK Letters and Months': 'cjk', 'CJK Compatibility': 'cjk',
    'Vertical Forms': 'cjk', 'CJK Compatibility Forms': 'cjk', 'Small Form Variants': 'cjk',
    'Halfwidth and Fullwidth Forms': 'cjk', 'Specials': 'symbols', 'Variation Selectors': 'latin',
    'Variation Selectors Supplement': 'latin', 'Alphabetic Presentation Forms': 'latin',
    'Mathematical Alphanumeric Symbols': 'math', 'Musical Symbols': 'music', 'Byzantine Musical Symbols': 'music',
    'Ancient Greek Musical Notation': 'music', 'Mahjong Tiles': 'emoji', 'Domino Tiles': 'symbols', 'Playing Cards': 'emoji',
    'Enclosed Alphanumeric Supplement': 'emoji', 'Enclosed Ideographic Supplement': 'cjk',
    'Miscellaneous Symbols and Pictographs': 'emoji', 'Emoticons': 'emoji', 'Ornamental Dingbats': 'symbols',
    'Transport and Map Symbols': 'emoji', 'Alchemical Symbols': 'symbols', 'Geometric Shapes Extended': 'emoji',
    'Supplemental Arrows-C': 'symbols', 'Supplemental Symbols and Pictographs': 'emoji',
    'Chess Symbols': 'symbols', 'Symbols and Pictographs Extended-A': 'emoji',
    'Symbols for Legacy Computing': 'symbols', 'Symbols for Legacy Computing Supplement': 'symbols',
    'Tags': 'latin', 'Private Use Area': 'none', 'Supplementary Private Use Area-A': 'none',
    'Supplementary Private Use Area-B': 'none', 'High Surrogates': 'none', 'Low Surrogates': 'none',
    'High Private Use Surrogates': 'none',
}

# Script scalars placed with a group other than their own script's.
SCRIPT_GROUPS = {
    'Latin': 'latin', 'Greek': 'latin', 'Cyrillic': 'latin', 'Common': 'latin', 'Inherited': 'latin',
    'Han': 'han', 'Hiragana': 'kana', 'Katakana': 'kana', 'Hangul': 'hangul', 'Bopomofo': 'bopomofo',
    'Braille': 'symbols', 'Zzzz': 'none',
}


def read_ranges(text):
    """Scripts.txt or Blocks.txt: [(first, last, value)]."""
    ranges = []
    for line in text.splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        span, value = [part.strip() for part in line.split(';', 1)]
        first, _, last = span.partition('..')
        ranges.append((int(first, 16), int(last or first, 16), value))
    return ranges


def noto_family(script):
    return NOTO_NAMES.get(script, 'Noto Sans ' + script.replace('_', ' '))


def dedupe(names):
    seen = set()
    result = []
    for name in names:
        if name not in seen:
            seen.add(name)
            result.append(name)
    return result


def cjk_chain(order, tail):
    """The preferred variant's Noto and distribution families, then one Noto CJK face of every other variant."""
    preferred = order[0]
    names = CJK_NOTO[preferred] + CJK_OTHER[preferred]
    for variant in order[1:]:
        names.append(CJK_NOTO[variant][0])
    return dedupe(names + tail)


def cjk_list(locale, extra_tail):
    return cjk_chain(CJK_ORDER[locale], extra_tail)


def family_list(group, locale):
    """The ordered target families of a group in one locale variant."""
    if group == 'none':
        return []
    if group == 'latin':
        return LATIN + ['Noto Sans Symbols', 'Noto Sans Math']
    if group == 'symbols':
        return dedupe(SYMBOLS + LATIN[1:3])
    if group == 'math':
        return dedupe(['Noto Sans Math', 'DejaVu Sans', 'Noto Sans Symbols', 'Noto Sans Symbols 2', 'FreeSerif'])
    if group == 'music':
        return dedupe(['Noto Music', 'Noto Sans Symbols 2', 'DejaVu Sans'])
    if group == 'emoji':
        return list(EMOJI)
    if group == 'cjk':
        return cjk_list(locale, TAIL)
    if group == 'han':
        return cjk_list(locale, TAIL)
    if group == 'kana':
        # Kana is drawn by the Japanese faces in every locale unless the locale prefers another.
        return cjk_chain(['ja', 'zh-cn', 'zh-tw', 'ko', 'zh-hk'], TAIL)
    if group == 'hangul':
        return cjk_chain(['ko', 'ja', 'zh-cn', 'zh-tw', 'zh-hk'], TAIL)
    if group == 'bopomofo':
        return cjk_chain(['zh-tw', 'zh-cn', 'ja', 'ko', 'zh-hk'], TAIL)
    # A script group: group is the UCD script name.
    names = [noto_family(group)]
    if group in NOTO_SERIF and not noto_family(group).startswith('Noto Serif'):
        names.append(noto_family(group).replace('Noto Sans', 'Noto Serif'))
    names += EXTRA.get(group, [])
    names += TAIL + LAST_RESORT[:1]
    return dedupe(names)


def classify(scripts, blocks):
    """Per scalar group: a script name, or one of the symbolic groups; 'none' when no family should be tried."""
    raw = [None] * LIMIT
    for first, last, script in scripts:
        for scalar in range(first, last + 1):
            raw[scalar] = script
    group = [SCRIPT_GROUPS.get(script, script) if script is not None else None for script in raw]
    for first, last, name in blocks:
        override = BLOCK_GROUPS.get(name)
        if override == 'none':
            for scalar in range(first, last + 1):
                group[scalar] = 'none'
        elif override is not None:
            # The Latin, Greek, Cyrillic, Common and Inherited scalars of a symbol or CJK block follow it (Common
            # symbols are not Latin text, the fullwidth Latin letters are drawn by the CJK families), and so do the
            # block's unassigned scalars. The scalars of every other script keep their script's families.
            for scalar in range(first, last + 1):
                if group[scalar] in ('latin', None):
                    group[scalar] = override
        else:
            # A block of one script (Arabic with its Common comma and question mark, Thai with its Common baht sign,
            # the holes between assigned scalars) is one interval: its Common, Inherited and unassigned scalars take
            # the families of the block's dominant script, so a word never crosses a routing boundary inside it.
            counts = {}
            for scalar in range(first, last + 1):
                if raw[scalar] not in (None, 'Common', 'Inherited'):
                    counts[raw[scalar]] = counts.get(raw[scalar], 0) + 1
            if counts:
                dominant = SCRIPT_GROUPS.get(max(counts, key=counts.get), max(counts, key=counts.get))
                for scalar in range(first, last + 1):
                    if raw[scalar] in (None, 'Common', 'Inherited'):
                        group[scalar] = dominant
    return group


def build(scripts, blocks):
    group = classify(scripts, blocks)
    # Runs of one group.
    runs = []
    start = 0
    for scalar in range(1, LIMIT + 1):
        if scalar == LIMIT or group[scalar] != group[start]:
            runs.append((start, group[start] if group[start] is not None else 'none'))
            start = scalar
    # Per locale, the family list of every run; adjacent runs that agree in every locale merge.
    def lists(name):
        per_locale = [family_list(name, locale) for locale in LOCALES]
        # One slot serves every locale, so the lists have one length: the shorter ones take families no variant
        # lists yet from the pad pool.
        length = max(len(items) for items in per_locale)
        for items in per_locale:
            for pad in PAD_POOL:
                if len(items) >= length:
                    break
                if pad not in items:
                    items.append(pad)
            assert len(items) == length, name
        return tuple(tuple(items) for items in per_locale)
    merged = []
    for first, name in runs:
        key = lists(name)
        if merged and merged[-1][1] == key:
            continue
        merged.append((first, key))
    return merged


def emit(merged, ucd_hash, script_hash):
    names = []
    name_index = {}

    def token(name):
        assert name.isascii() and 0 < len(name) < 64
        if name not in name_index:
            name_index[name] = len(names)
            names.append(name)
        return name_index[name]

    # Slots: one per distinct (list per locale) key, so a CJK group has six token lists of the same length.
    slots = {}
    ranges = []
    tokens = [[] for _ in LOCALES]
    for first, key in merged:
        count = len(key[0])
        assert all(len(item) == count for item in key), 'a locale variant changes the length of a list'
        assert count <= MAX_FAMILIES, (first, count)
        if key not in slots:
            slots[key] = len(tokens[0])
            for locale_index, items in enumerate(key):
                tokens[locale_index] += [token(name) for name in items]
        ranges.append((first, slots[key], count))
    lines = []
    lines.append('// Linux system fallback policy: tools/font_reference/linux_fallback.py, from Unicode 17.0.0')
    lines.append('// Scripts.txt and Blocks.txt. A written policy, not an observed one: native_system_fallback.inc consults')
    lines.append('// fontconfig first and appends these families to what it returns.')
    lines.append('// UCD archive SHA-256: %s' % ucd_hash)
    lines.append('// Generator SHA-256: %s' % script_hash)
    lines.append('constexpr NativeFallbackRange system_fallback_ranges[]{')
    for first, offset, count in ranges:
        lines.append('    {0x%x,%d,%d},' % (first, offset, count))
    lines.append('};')
    # A token is the offset of its family's name in system_fallback_names (the name runs to its NUL). The six token
    # lists (unspecified language, ja, ko, zh-cn, zh-tw, zh-hk) have the same layout, so the ranges above serve all of
    # them, and they differ only in the CJK slots: one list holds the rest and the variants cover that span.
    offsets = []
    position = 0
    for name in names:
        offsets.append(position)
        position += len(name) + 1
    tokens = [[offsets[token] for token in row] for row in tokens]
    differing = [i for i in range(len(tokens[0])) if any(row[i] != tokens[0][i] for row in tokens)]
    variant_first, variant_end = differing[0], differing[-1] + 1
    lines.append('constexpr u16 system_fallback_tokens[%d]{%s};' % (len(tokens[0]), ','.join(str(value) for value in tokens[0])))
    lines.append('constexpr u32 system_fallback_variant_first=%d;' % variant_first)
    lines.append('constexpr u32 system_fallback_variant_count=%d;' % (variant_end - variant_first))
    lines.append('constexpr u16 system_fallback_variants[%d][%d]{' % (len(LOCALES), variant_end - variant_first))
    for locale_index, locale in enumerate(LOCALES):
        body = ','.join(str(value) for value in tokens[locale_index][variant_first:variant_end])
        lines.append('    {%s}, // %s' % (body, locale))
    lines.append('};')
    lines.append('constexpr u32 system_fallback_locale_count=%d;' % len(LOCALES))
    lines.append('constexpr char system_fallback_names[]=')
    for name in names:
        lines.append('    "%s\\0"' % name)
    lines.append('    ;')
    lines.append('constexpr u32 system_fallback_max_families=%d;' % MAX_FAMILIES)
    lines.append('static_assert([]() constexpr {')
    lines.append('    u32 run=0;')
    lines.append('    for(u32 i=0;i+1<sizeof(system_fallback_names);++i) {')
    lines.append('        const u8 c=static_cast<u8>(system_fallback_names[i]);')
    lines.append('        if(c>=128)return false;')
    lines.append('        if(c){if(++run>=64)return false;}else{if(!run)return false;run=0;}')
    lines.append('    }return run==0;')
    lines.append('}());')
    lines.append('constexpr bool system_fallback_token_valid(u32 token) noexcept {')
    lines.append('    return token<sizeof(system_fallback_names)-1&&system_fallback_names[token]&&(!token||!system_fallback_names[token-1]);')
    lines.append('}')
    lines.append('static_assert([]() constexpr {')
    lines.append('    for(const u16 token:system_fallback_tokens)if(!system_fallback_token_valid(token))return false;')
    lines.append('    for(const auto& row:system_fallback_variants)for(const u16 token:row)if(!system_fallback_token_valid(token))return false;')
    lines.append('    return true;')
    lines.append('}());')
    lines.append('constexpr u32 system_fallback_range_count=%d;' % len(ranges))
    lines.append('constexpr u32 system_fallback_token_count=%d;' % len(tokens[0]))
    lines.append('constexpr NativeFallbackCascade system_fallback_policy=[]() constexpr {')
    lines.append('    NativeFallbackCascade policy{};')
    lines.append('    native_fallback_cascade_open({system_fallback_ranges,system_fallback_range_count},{system_fallback_tokens,system_fallback_token_count},sizeof(system_fallback_names),&policy);')
    lines.append('    return policy;')
    lines.append('}();')
    lines.append('static_assert(system_fallback_policy.ranges.count==system_fallback_range_count);')
    lines.append('// The token at `index` of the lists of one CJK order.')
    lines.append('constexpr u32 system_fallback_token(u32 locale,u32 index) noexcept {')
    lines.append('    const u32 at=index-system_fallback_variant_first;')
    lines.append('    return at<system_fallback_variant_count?system_fallback_variants[locale][at]:system_fallback_tokens[index];')
    lines.append('}')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ucd', type=pathlib.Path, required=True, help='the pinned UCD.zip of Unicode 17.0.0')
    parser.add_argument('--output', type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[2] / 'src/ui/text/generated/linux_fallback_data.inc')
    args = parser.parse_args()
    data = args.ucd.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_UCD_SHA256:
        raise SystemExit('UCD archive does not match the pinned Unicode 17.0.0 release')
    with zipfile.ZipFile(args.ucd) as archive:
        scripts = read_ranges(archive.read('Scripts.txt').decode('utf-8'))
        blocks = read_ranges(archive.read('Blocks.txt').decode('utf-8'))
    script_hash = hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()
    text = emit(build(scripts, blocks), digest, script_hash)
    args.output.write_text(text, encoding='utf-8', newline='\n')
    print('%s: %d bytes' % (args.output, len(text)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
