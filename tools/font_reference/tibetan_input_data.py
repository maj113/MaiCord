"""Author the compact Tibetan frontend classes; no Windows table is embedded."""
from pathlib import Path
NAMES=('other','base','placeholder','symbol','reserved','subjoined','cons_above','cons_below',
       'vowel_pre','vowel_above','vowel_below','compound','vowel_post','modifier','final','split')
def categories():
    values=['reserved']*256
    def put(kind,*items):
        for item in items:
            for c in item if isinstance(item,range) else [item]:values[c-0xf00]=kind
    put('other',0xf02,0xf03,range(0xf07,0xf15),range(0xf3a,0xf3e),0xf7f,range(0xfd0,0xfd5),0xfd9,0xfda)
    put('base',0xf00,0xf01,range(0xf04,0xf07),range(0xf40,0xf48),range(0xf49,0xf6d))
    put('placeholder',range(0xf20,0xf34),0xf85,range(0xf88,0xf8d))
    put('symbol',range(0xf15,0xf18),range(0xf1a,0xf20),0xf34,0xf36,0xf38,
        range(0xfbe,0xfc6),range(0xfc7,0xfcd),0xfce,0xfcf,range(0xfd5,0xfd9))
    put('subjoined',range(0xf8d,0xf98),range(0xf99,0xfbd))
    put('cons_above',0xf39);put('cons_below',0xf71)
    put('vowel_pre',0xf3f);put('vowel_above',0xf74)
    put('vowel_below',0xf18,0xf19,0xf72,0xf75,range(0xf7a,0xf7e),0xf80,0xf84)
    put('compound',0xf77,0xf79);put('split',0xf73,0xf76,0xf78,0xf81)
    put('vowel_post',0xf3e);put('modifier',0xf7e,0xf82,0xf83,0xf86,0xf87)
    put('final',0xf35,0xf37,0xfc6)
    return [NAMES.index(v) for v in values]
def main():
    values=categories();packed=[a|(b<<4) for a,b in zip(values[::2],values[1::2])]
    lines=['// Authored by tools/font_reference/tibetan_input_data.py. Two categories per byte.']
    for at in range(0,128,8):lines.append('    '+','.join(f'0x{v:02x}' for v in packed[at:at+8])+f', // U+{0xf00+at*2:04X}')
    Path('src/text/generated/tibetan_categories.inc').write_text('\n'.join(lines)+'\n',encoding='utf-8')
if __name__=='__main__':main()
