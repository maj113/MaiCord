"""Authored Khmer boundary and vowel-preparation cases; offline QA only."""
import itertools,random

def corpus(stress=False):
    cases={}
    def add(s,group):
        s=tuple(map(ord,s)) if isinstance(s,str) else tuple(s)
        if s:cases.setdefault(s,group)
    alphabet=(0x1780,0x179a,0x1799,0x17a5,0x17d2,0x17b6,0x17b7,0x17bb,
              0x17be,0x17c1,0x17c4,0x17c6,0x17c7,0x17c9,0x17cc,0x17d3,
              0x17dd,0x200c,0x200d,0x34f,0xfe00)
    if stress:
        rng=random.Random(20260920)
        for _ in range(4096):
            add((rng.choice(alphabet+(0x17b4,0x17b5,0x17e0,0xa0,0x25cc,0x20)) for _ in range(rng.randint(1,72))),'holdout')
        return list(cases.items())
    for c in range(0x1780,0x1800):
        for s in ((c,),(0x1780,c),(c,0x17d2,0x1780),(0x1780,c,0x17c1),(0x1780,c,c)):
            add(s,'scalar-coverage')
    for base in (0x1780,0x179a,0x17a5,0x17e0,0xa0,0x25cc,0x17c1,0x200d):
        for pair in itertools.product(alphabet,repeat=2):add((base,*pair),'pairs')
    for prefix in ((0x1780,0x17d2),(0x1780,0x17c1),(0x1780,0x17c9),
                   (0x1780,0x17d2,0x179a),(0x1780,0x17d2,0x1780),
                   (0x1780,0x17d2,0x1799),(0x1780,0x17d2,0x200d),
                   (0x1780,0x17d2,0x200c)):
        for pair in itertools.product(alphabet,repeat=2):add((*prefix,*pair),'tails')
    for base in range(0x1780,0x17b6):
        for sub in range(0x1780,0x17b6):add((0x1780,0x17d2,base,0x17d2,sub),'subscript-types')
    for word in 'ភាសាខ្មែរ សួស្តី កម្ពុជា ភ្នំពេញ ព័ត៌មាន កុំព្យូទ័រ សារថ្មី សូមអរគុណ'.split():
        for end in range(1,len(word)+1):add(word[:end],'words-and-partial-input')
    for sign in alphabet[4:]:
        for n in (29,30,31,32,64):add((0x1780,)+(sign,)*n,'capacity')
    for link in ((0x17d2,),(0x17d2,0x200d),(0x200d,),(0x17d2,0x200c)):
        add((0x1780,*link)*40+(0x17c1,),'capacity')
    return list(cases.items())
