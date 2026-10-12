"""First-party Odia original-cluster inputs; no font payload."""
CONSONANTS=(*range(0xb15,0xb29),*range(0xb2a,0xb31),0xb32,0xb33,
            *range(0xb35,0xb3a),0xb5c,0xb5d,0xb5f,0xb71)
SIGNS=(0xb3c,0xb3e,0xb3f,0xb40,0xb41,0xb42,0xb43,0xb44,0xb47,0xb48,
       0xb4b,0xb4c,0xb4d,0xb01,0xb02,0xb03,0xb55,0xb56,0xb57,0xb62,0xb63)


def corpus():
    cases={}
    def add(s,g):cases.setdefault(tuple(s),g)
    for c in CONSONANTS:
        add((c,),'nominal')
        for sign in SIGNS:add((c,sign),'signs')
        for other in CONSONANTS:
            add((c,0xb4d,other),'conjuncts')
            add((c,0xb4d,other,0xb47),'conjunct-pre-vowel')
            add((c,0xb4d,other,0xb4c),'conjunct-split-vowel')
        for control in (0x200c,0x200d):add((c,0xb4d,control),'controls')
    for sign in SIGNS:add((sign,),'orphans')
    for scalar in (0xb05,0xb06,0xb07,0xb08,0xb09,0xb0a,0xb0b,0xb0c,0xb0f,0xb10,0xb13,0xb14,0xb60,0xb61):
        add((scalar,),'independent')
    return list(cases.items())


def complex_corpus():
    import itertools
    cases={}
    def add(s,g):cases.setdefault(tuple(s),g)
    bases=(0xb15,0xb1c,0xb24,0xb26,0xb2f,0xb30,0xb35,0xb39)
    signs=(0xb3f,0xb41,0xb47,0xb48,0xb4b,0xb4c,0xb3e,0xb40)
    for i,(a,b,c) in enumerate(itertools.product(bases,repeat=3)):
        add((a,0xb4d,b,0xb4d,c,signs[i%len(signs)]),'three-consonants')
    for base in bases:
        for sign in signs:
            for prefix in ((base,0xb3c),(base,0xb4d,0xb3c,0xb15),(0xb30,0xb4d,base),
                           (base,0xb4d,0x200d,0xb15),(base,0xb4d,0xb15),(base,0xb4d,0xb2f)):
                add((*prefix,sign,0xb01),'marks-and-controls')
            for extra in (0xb3e,0xb40):add((base,sign,extra),'multiple-matras')
    return list(cases.items())


def capacity_corpus():
    cases=[]
    for prefix in ((0xb15,),(0x200d,0xb4d,0xb15)):
        for tail in ((0xb4b,),(0xb4c,),(0xb4b,0xb4c)):
            for count in range(27,32):
                cases.append((prefix+(0xb3e,)*(count-len(prefix)-len(tail))+tail,'preparation-capacity'))
    return cases


def modifier_corpus():
    cases=[]
    for overlines in range(5):
        for count in range(1,10):
            tail=tuple(0xb40 if i%2 else 0xb3e for i in range(count))
            cases.append(((0xb15,)+(0xb55,)*overlines+tail+(0xb01,),'terminal-bindu'))
    tails=((0xb02,),(0xb03,),(0xb02,0xb02),(0xb02,0xb03),(0xb03,0xb03),
           (0xb02,0x200c),(0xb03,0x200c),(0xb01,),(0xb01,0xb02))
    for prefix in ((),(0xb3f,),(0xb41,),(0xb3e,),(0xb47,),(0xb4b,),(0xb3f,0xb41)):
        for overlines in range(1,4):
            for tail in tails:
                cases.append(((0xb15,)+prefix+(0xb55,)*overlines+tail,'overline-modifiers'))
    for prefix in ((),(0xb3f,),(0xb41,),(0xb47,)):
        for overlines in range(1,4):
            for nukta in ((),(0xb3c,)):
                for post in ((),(0xb57,)):
                    cases.append(((0xb30,)+prefix+(0xb55,)*overlines+nukta+post+(0xb03,),'overline-visarga'))
    return cases
