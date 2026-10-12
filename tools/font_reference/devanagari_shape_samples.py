"""First-party short shaping inputs; no font data or expected output is embedded."""
from pathlib import Path


def samples():
    words = ('नमस्ते हिन्दी संस्कृत मराठी क्षत्रिय प्रार्थना श्री श्रद्धा राष्ट्र कर्तृत्व '
             'विद्यार्थी ज्ञान दृष्टि स्त्री क्रान्ति सूर्य पृथ्वी कृपया कम्प्यूटर दिल्ली '
             'हूँ दुःख क़िला ख़ुशी फ़ोन ज़रूरी ऋषि ॐ').split()
    cases = dict.fromkeys(word[:end] for word in words for end in range(1, len(word)+1))
    for text in ('कि', 'र्क', 'र्का', 'र्कि', 'र्कु', 'र्के', 'र्कँ', 'क्र', 'द्मि', 'द्कि',
                 'र्द्कि', 'क्\u200d', 'र्\u200d', 'क्\u200c', 'र्\u200dकि', 'क्\u200cषि',
                 'क्\u200dषि', 'क़', 'क़', 'ऩ', 'ऱ', 'ऴ', 'ि', '्', 'ँ', 'िि', 'क़्',
                 'क़़', 'किं', 'कः', 'क॑', 'क॒', 'क॒॑', 'कँः', '\u200d', '\u200dि', '\u200d्क',
                 '\u200d\u200c', 'कि़', 'कि़्', 'क़्\u200d', 'क्\u200dर', 'क्ँ\u200d', 'क्ँ\u200c',
                 'ऱ्र', 'र\u200c्कि', 'र\u200c\u200dकि', 'किँ\u200c', 'किीीँ\u200c',
                 'कि़्ँ\u200c', 'अ्\u200dकि', 'अ्कि', 'र्अ्क॑', 'अा', 'आे', 'एे', 'अ़ा', 'अ\u200dा'):
        cases[text] = None
    return list(cases)


if __name__ == '__main__':
    lines = ['// First-party corpus. Regenerate with devanagari_shape_samples.py.',
             'constexpr const wchar_t* deva_shape_samples[]{']
    lines += ['    L"' + ''.join(f'\\u{ord(c):04x}' for c in text) + '",' for text in samples()]
    lines.append('};')
    path = Path('tests/generated/devanagari_shape_samples.inc')
    path.write_text('\n'.join(lines)+'\n', encoding='utf-8')
    print(f'Wrote {len(samples())} shaping inputs')
