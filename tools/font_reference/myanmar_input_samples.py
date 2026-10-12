"""Independent authored Myanmar boundary corpus; no native expectations."""
import itertools, random

ALPHABET=(0x1000,0x1004,0x1021,0x1040,0x1041,0x1039,0x103a,0x103b,0x103c,0x103d,0x103e,
          0x1031,0x102d,0x102f,0x1036,0x1037,0x102b,0x102c,0x1063,0x1038,
          0x200c,0x200d,0xfe00,0xfe01,0x34f,0x2060,0x20,0xa0,0x41)
def corpus(stress=False):
    cases={}
    def add(s,group):
        s=tuple(s)
        if s:cases.setdefault(s,group)
    scalars=list(range(0x1000,0x10a0))+list(range(0xaa60,0xaa80))+list(range(0xa9e0,0xaa00))
    for c in scalars:
        for prefix in ((),(0x1000,),(0x1031,),(0x1000,0x1039)):
            add((*prefix,c),'scalar-coverage')
    for head in ((),(0x1000,),(0x1031,),(0x1039,),(0x1040,),(0x25cc,),
                 (0x1004,0x103a,0x1039),(0x1000,0x102c),(0x1000,0x1063),(0x1000,0x103d)):
        for pair in itertools.product(ALPHABET,repeat=2):add((*head,*pair),'structural-pairs')
    for word in 'မြန်မာဘာသာ မင်္ဂလာပါ ကျေးဇူးတင်ပါတယ် ကွန်ပျူတာ သတင်းစကား င်္က္ကျြွှေို့်ာှီ့ၤဲံ့းႍ'.split():
        for end in range(1,len(word)+1):add(map(ord,word[:end]),'partial-word')
    for sign in ALPHABET:
        for n in (30,31,32,33,62,64,100,200):
            add((0x1000,)+(sign,)*n,'long')
    rng=random.Random(60021)
    for _ in range(3000 if not stress else 20000):
        add([rng.choice(ALPHABET) for i in range(rng.randint(1,24))],'random')
    for c in (0xfe00,0xfe01,0xe0100,0x1f600):
        for head in (0x1000,0x1039,0x103a,0x1031,0x1040,0x20):
            add((head,c,0x1000),'selectors');add((head,c,c,0x1031),'selectors')
    if stress:
        full=(0x1000,0x1039,0x1000,0x103a,0x103b,0x103c,0x103d,0x103e,
              0x1031,0x102d,0x102f,0x1036,0x1037,0x103a,0x102c,0x103e,
              0x103a,0x102d,0x1036,0x1037,0x103a,0x1063,0x1036,0x1037,0x103a,0x1038)
        for end in range(1,len(full)+1):
            for tail in itertools.product(ALPHABET,repeat=2):add((*full[:end],*tail),'deep-tails')
        for kinzi in (0x1004,0x101b,0x105a):
            for first in ALPHABET:
                for control in (0xfe00,0xe0100,0x34f):
                    for at in range(5):
                        s=[kinzi,0x103a,0x1039,first,0x1031];s.insert(at,control);add(s,'kinzi-controls')
        for n in (29,30,31,32,33,63):
            for mark in (0x1031,0x103a,0x102c,0x1063,0x34f):
                for selector in (0xfe00,0xe0100):add((0x1000,)+(mark,selector)*n+(0x1000,),'long-selectors')
    return list(cases.items())
