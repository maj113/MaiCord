bool native_bidi_format_controls(DWriteTextSystem system) noexcept {
    const u32 controls[]{0x202a,0x202b,0x202c,0x202d,0x202e,0x2066,0x2067,0x2068,0x2069};
    auto native=system,reference=system;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    for(u32 length=1,total=9;length<=3;++length,total*=9)for(u32 word=0;word<total;++word)for(u32 context=0;context<(length<3?2u:1u);++context){char source[64];u32 bytes=0,value=word;
        if(context){source[bytes++]='A';source[bytes++]=' ';}
        for(u32 i=0;i<length;++i){bytes+=utf8_encode(controls[value%9],source+bytes);value/=9;}
        if(context){const auto suffix=utf8_literal(" हिन्दी אב");copy_bytes(source+bytes,suffix.data,suffix.bytes);bytes+=suffix.bytes;}
        const Utf8View text{source,bytes};const float width=context?37.f:1000000.f;++cases;
        CHECK(text_shape_utf8(&reference,text,{},width,1000000,{utf16,256},&reference_output.text).ok());
        CHECK(text_shape_utf8(&native,text,{},width,1000000,{utf16,256},&native_output.text).ok()&&native_output.text.native_fonts());
        if(!same(native_output,reference_output,true)||!compare_hits(system,text,width,TextWrapping::wrap,{},true,devanagari_tracking)){
            print("bidi-format length/word/context=");number(length);print("/");number(word);print("/");number(context);print("\n");return false;}++native_cases;
    }return true;
}
bool native_devanagari_corpus(DWriteTextSystem system) noexcept {
    const Utf8View tibetan[]{utf8_literal("བོད་ཡིག བཀྲ་ཤིས"),utf8_literal("ཀི ཀུ ཀཱི ཀྲྀ ཀླྀ ཀཱྀ"),
        utf8_literal("བསྒྲུབས ཀྐི སྐྱ"),utf8_literal("ཀ\u200d ཀ\u200c ཀ\u034fི"),
        utf8_literal("ཀཱི ཀཱི ཀཱྀ ཀཱྀ"),utf8_literal("ཀཷ ཀཹ ཀཱུ ཿ"),utf8_literal("ཨོཾ མཎི པདྨེ ཧཱུྃ"),
        utf8_literal("ི ཱི ྐ ༿ ཀ༿"),utf8_literal("ཀ\ufe00 ཀཱི\ufe00 ཀ\U000e0100"),
        utf8_literal("བོད\tཡིག\r\nབཀྲ་ཤིས"),utf8_literal("A བོད་ཡིག 😀 B"),utf8_literal("བོད אב العربية ཡིག"),
        utf8_literal("A \u202eབོད ཀཱི\u202c B"),utf8_literal("བོད 😀 X"),utf8_literal("༠༡༢ བོད། ཡིག༎"),
        utf8_literal("བོད་ཡིག བཀྲ་ཤིས་བདེ་ལེགས བོད་ཡིག")};
    const Utf8View myanmar[]{utf8_literal("မြန်မာ ဘာသာ"),utf8_literal("ကိ ကီ ကု ကူ ကေ ကို"),utf8_literal("မင်္ဂလာပါ ကမ္ဘာ"),
        utf8_literal("င်္ကေ ကျ ကြ ကွှ"),utf8_literal("မြန်မာ\r\nဘာသာ"),utf8_literal("A မြန်မာ 😀 B"),
        utf8_literal("ေ ိ ု ္ ် ြ"),utf8_literal("(မြန်မာ) [ဘာသာ]"),utf8_literal("မြန်မာ\tဘာသာ  "),
        utf8_literal("မြန်မာ ខ្មែរ සිංහල தமிழ் हिन्दी"),utf8_literal("\u202eမြန်မာ ကေ ကို\u202c"),utf8_literal("אב မြန်မာ ဘာသာ גד"),
        utf8_literal("\u202eင်္ကေ ကျွှ\u202c"),utf8_literal("ကော ကျောင်း ကြွေ"),
        utf8_literal("က\u200d က\u200c က\u034f က္က"),utf8_literal("က\ufe00 က\U000e0100 ကံ့ က့ံ")};
    const Utf8View khmer[]{utf8_literal("ភាសាខ្មែរ កម្ពុជា"),utf8_literal("កិ កី កុ កូ កើ កោ"),utf8_literal("សួស្តី ពិភពលោក"),
        utf8_literal("ក្រ ក្ក ក្រ្ក ក្ក្រ"),utf8_literal("ភាសាខ្មែរ\r\nកម្ពុជា"),utf8_literal("A ខ្មែរ 😀 B"),
        utf8_literal("េ ើ ោ ៅ ្ ៌"),utf8_literal("(ខ្មែរ) [កម្ពុជា]"),utf8_literal("ខ្មែរ\tកម្ពុជា  "),
        utf8_literal("ខ្មែរ සිංහල മലയാളം தமிழ் हिन्दी"),utf8_literal("\u202eខ្មែរ កើ កោ\u202c"),utf8_literal("אב ខ្មែរ កម្ពុជា גד"),
        utf8_literal("\u202eក្រ្ក ក្ក្រ\u202c"),utf8_literal("កេ កែ កៃ កើ កឿ កៀ កោ កៅ"),
        utf8_literal("ក\u200d ក\u200c ក\u034f ក្ក"),utf8_literal("ក\u17b4 ក\u17b5 ក\ufe00 ក\U000e0100")};
    const Utf8View sinhala[]{utf8_literal("සිංහල භාෂාව"),utf8_literal("කි කී කු කූ"),utf8_literal("ශ්‍රී ලංකාව"),
        utf8_literal("ක්‍ර ක‍්ර ක‌ර"),utf8_literal("සිංහල\r\nභාෂාව"),utf8_literal("A සිංහල 😀 B"),
        utf8_literal("ෙ ේ ො ෝ ෞ ්"),utf8_literal("(සිංහල) [භාෂාව]"),utf8_literal("සිංහල\tභාෂාව  "),
        utf8_literal("සිංහල മലയാളം தமிழ் हिन्दी"),utf8_literal("\u202eසිංහල කි කො\u202c"),utf8_literal("אב සිංහල භාෂාව גד"),
        utf8_literal("\u202eර්‍කෙ ක්‍ර\u202c"),utf8_literal("කෙ කේ කො කෝ කෞ"),utf8_literal("කො කෝ කෞ"),
        utf8_literal("ක\u034f කෙ\u034f ක්‍ක ක්‌ක")};
    const Utf8View malayalam[]{utf8_literal("മലയാളം ഭാഷ"),
        utf8_literal("കി കീ കു കൂ കൈ"),
        utf8_literal("ക്ഷേത്രം ശ്രദ്ധ"),
        utf8_literal("ക്‍ഷ ക്‌ഷ"),
        utf8_literal("മലയാളം\r\nഭാഷ"),
        utf8_literal("A മലയാളം 😀 B"),
        utf8_literal("ി ൈ ് ൗ"),
        utf8_literal("(മലയാളം) [ഭാഷ]"),
        utf8_literal("മലയാളം\tഭാഷ  "),
        utf8_literal("മലയാളം ಕನ್ನಡ తెలుగు ଓଡ଼ିଆ ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ்"),
        utf8_literal("‮മലയാളം കി കൈ‬"),
        utf8_literal("אב മലയാളം ഭാഷ גד"),
        utf8_literal("‮ക്‍ഷ ക്‌ഷ‬"),
        utf8_literal("ർ ൻ ൽ ൾ ൺ ൿ ൎകി"),
        utf8_literal("കൊ കോ കൗ കൊ കോ കൌ")};
    const Utf8View kannada[]{utf8_literal("ಕನ್ನಡ ಭಾಷೆ"),utf8_literal("ಕಿ ಕೀ ಕು ಕೂ ಕೈ"),utf8_literal("ಕ್ಷೇತ್ರ ಶ್ರದ್ಧೆ"),
        utf8_literal("ಕ್‍ಷ ಕ್‌ಷ"),utf8_literal("ಕನ್ನಡ\r\nಭಾಷೆ"),utf8_literal("A ಕನ್ನಡ 😀 B"),
        utf8_literal("ಿ ೈ ್ ೕ"),utf8_literal("(ಕನ್ನಡ) [ಭಾಷೆ]"),utf8_literal("ಕನ್ನಡ\tಭಾಷೆ  "),
        utf8_literal("ಕನ್ನಡ తెలుగు ଓଡ଼ିଆ ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ்"),utf8_literal("\u202eಕನ್ನಡ ಕಿ ಕೈ\u202c"),utf8_literal("אב ಕನ್ನಡ ಭಾಷೆ גד"),
        utf8_literal("\u202eಕ್‍ಷ ಕ್‌ಷ\u202c"),utf8_literal("ರ್ಕಿ ರ್ಕೋ ಕ್ರೈ ಕ್‍ಕಿ"),utf8_literal("ಕೊ ಕೋ ಕೌ ಕೃ ಕೄ ಕೊ ಕೋ")};
    const Utf8View telugu[]{utf8_literal("తెలుగు భాష"),utf8_literal("కి కీ కు కూ కై"),utf8_literal("క్షేత్రం శ్రద్ధ"),
        utf8_literal("క్‍ష క్‌ష"),utf8_literal("తెలుగు\r\nభాష"),utf8_literal("A తెలుగు 😀 B"),
        utf8_literal("ి ై ్ ౕ"),utf8_literal("(తెలుగు) [భాష]"),utf8_literal("తెలుగు\tభాష  "),
        utf8_literal("తెలుగు ଓଡ଼ିଆ ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ்"),utf8_literal("\u202eతెలుగు కి కై\u202c"),utf8_literal("אב తెలుగు భాష גד"),
        utf8_literal("\u202eక్‍ష క్‌ష\u202c"),utf8_literal("క్రై క్ర్రై క్‍కి"),utf8_literal("కొ కౌ కృ కౄ కాు కైు కైంఁఁ‌")};
    const Utf8View odia[]{utf8_literal("ଓଡ଼ିଆ ଭାଷା"),utf8_literal("କି କୀ କୁ କୂ କୈ"),utf8_literal("କ୍ଷେତ୍ର ଶ୍ରଦ୍ଧା"),
        utf8_literal("କ୍‍ଷ କ୍‌ଷ"),utf8_literal("ଓଡ଼ିଆ\r\nଭାଷା"),utf8_literal("A ଓଡ଼ିଆ 😀 B"),
        utf8_literal("ି ୌ ୍ ୕"),utf8_literal("(ଓଡ଼ିଆ) [ଭାଷା]"),utf8_literal("ଓଡ଼ିଆ\tଭାଷା  "),
        utf8_literal("ଓଡ଼ିଆ ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ்"),utf8_literal("\u202eଓଡ଼ିଆ କି କୌ\u202c"),utf8_literal("אב ଓଡ଼ିଆ ଭାଷା גד"),
        utf8_literal("\u202eକ୍‍ଷ କ୍‌ଷ\u202c"),utf8_literal("ର୍କି କ୍ଯୁଁ କ୍କୁଁ"),utf8_literal("ରୁୀଁ କେ୕ୗଃ କ୕୍୕ୗ")};
    const Utf8View gurmukhi[]{utf8_literal("ਪੰਜਾਬੀ ਭਾਸ਼ਾ"),utf8_literal("ਕਿ ਕੀ ਕੁ ਕੂ ਕੈ"),utf8_literal("ਪ੍ਰਾਰਥਨਾ ਸ਼ਰਧਾ"),
        utf8_literal("ਕ੍‍ਸ਼ ਕ੍‌ਸ਼"),utf8_literal("ਪੰਜਾਬੀ\r\nਭਾਸ਼ਾ"),utf8_literal("A ਪੰਜਾਬੀ 😀 B"),
        utf8_literal("ਿ ੍ ੑ ੵ"),utf8_literal("(ਪੰਜਾਬੀ) [ਭਾਸ਼ਾ]"),utf8_literal("ਪੰਜਾਬੀ\tਭਾਸ਼ਾ  "),
        utf8_literal("ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ்"),utf8_literal("\u202eਪੰਜਾਬੀ ਕਿ ਕੂ\u202c"),utf8_literal("אב ਪੰਜਾਬੀ ਭਾਸ਼ਾ גד"),
        utf8_literal("\u202eਕ੍‍ਸ਼ ਕ੍‌ਸ਼\u202c"),utf8_literal("ਕ੍ਕਿ ਕ੍ਯ ਕ੍ਰ"),utf8_literal("ਕੑ ਕੵ ੲ ੳ ਕੰੱ ਕੀਂ")};
    const Utf8View gujarati[]{utf8_literal("ગુજરાતી ભાષા"),utf8_literal("કિ કો કૌ ર્કિ"),utf8_literal("ક્ષેત્ર શ્રદ્ધા"),
        utf8_literal("ક્‍ષ ક્‌ષ"),utf8_literal("ગુજરાતી\r\nભાષા"),utf8_literal("A ગુજરાતી 😀 B"),
        utf8_literal("િ ૉ ્"),utf8_literal("(ગુજરાતી) [ભાષા]"),utf8_literal("ગુજરાતી\tભાષા  "),
        utf8_literal("ગુજરાતી हिन्दी বাংলা தமிழ்"),utf8_literal("\u202eગુજરાતી કો કૌ\u202c"),utf8_literal("אב ગુજરાતી ભાષા גד"),
        utf8_literal("\u202eક્‍ષ ક્‌ષ\u202c"),utf8_literal("ક઼્કિ ર઼્કિ"),utf8_literal("ક્\u0afd ક્\u0afe ક્\u0aff ક્ઁ\u200c")};
    const Utf8View bengali[]{utf8_literal("বাংলা ভাষা"),utf8_literal("কো কৌ র্কি"),utf8_literal("ক্ষেত্র শ্রদ্ধা"),
        utf8_literal("ক্‍ষ ক্‌ষ"),utf8_literal("বাংলা\r\nভাষা"),utf8_literal("A বাংলা 😀 B"),
        utf8_literal("ে ৌ ্"),utf8_literal("(বাংলা) [ভাষা]"),utf8_literal("বাংলা\tভাষা  "),utf8_literal("বাংলা हिन्दी தமிழ்"),
        utf8_literal("\u202eবাংলা কো কৌ\u202c"),utf8_literal("אב বাংলা ভাষা גד"),utf8_literal("\u202eক্‍ষ ক্‌ষ\u202c")};
    const Utf8View tamil[]{utf8_literal("தமிழ் மொழி"),utf8_literal("கொ கோ கௌ"),utf8_literal("க்ஷே ஸ்ரீ"),
        utf8_literal("க்‍க க்‌க"),utf8_literal("தமிழ்\r\nவணக்கம்"),utf8_literal("A தமிழ் 😀 B"),
        utf8_literal("கொ கோ கௌ"),utf8_literal("ெ ௌ ்"),utf8_literal("கி கீ கு கூ கை"),utf8_literal("ஔ ஔ"),
        utf8_literal("தமிழ் हिन्दी தமிழ்"),utf8_literal("(தமிழ்) [மொழி]"),utf8_literal("தமிழ்\tமொழி  ")};
    const Utf8View tamil_direction[]{utf8_literal("\u202eதமிழ் மொழி\u202c"),utf8_literal("A \u202eகொ கோ கௌ\u202c B"),
        utf8_literal("אב தமிழ் மொழி גד"),utf8_literal("\u202eதமிழ் 😀 மொழி\u202c"),utf8_literal("\u202eக்‍க க்‌க\u202c"),
        utf8_literal("\u202e(தமிழ்) [மொழி]\u202c"),utf8_literal("\u2067தமிழ் \u202eகொ\u202c\u2069"),
        utf8_literal("\u202eதமிழ்\r\nமொழி\u202c"),utf8_literal("\u202eெ ௌ ்\u202c"),utf8_literal("தமிழ் கொ கோ கௌ")};
    const Utf8View ordinary[]{utf8_literal("नमस्ते दुनिया"),utf8_literal("र्कि कि क़ि"),utf8_literal("क्षेत्र हिन्दी संस्कृत"),
        utf8_literal("क्‍ष क्‌ष"),utf8_literal("भारत\r\nहिन्दी"),utf8_literal("A हिन्दी 😀 B"),
        utf8_literal("ॐ ऋषि किँ कः"),utf8_literal("ि ़ ॑ क॑"),utf8_literal("र्कि किं कृ त्र ज्ञ"),utf8_literal("क़ ख़ ग़ ज़ ड़ ढ़ फ़ य़")};
    const Utf8View directional[]{utf8_literal("\u202eर्कि हिन्दी क्षेत्र\u202c"),utf8_literal("A \u202eर्कि कि क़ि\u202c B"),
        utf8_literal("אב हिन्दी क्षेत्र גד"),utf8_literal("\u202eनमस्ते 😀 दुनिया\u202c"),utf8_literal("\u202eक्‍ष क्‌ष\u202c"),
        utf8_literal("\u202e(र्कि) [क्षेत्र]\u202c"),utf8_literal("\u2067हिन्दी \u202eर्कि\u202c\u2069"),
        utf8_literal("\u202eहिन्दी\r\nकिं\u202c"),utf8_literal("\u202eर्कि किं कृ त्र ज्ञ"),utf8_literal("A \u202eक़ ख़ ग़ ज़ ड़ ढ़ फ़ य़\u202c B"),
        utf8_literal("A \u202aאב\u202c B"),utf8_literal("\u202bאב हिन्दी\u202c"),utf8_literal("\u202dאב हिन्दी\u202c"),
        utf8_literal("\u202eAV ffi\u202c"),utf8_literal("\u2067אב\u2069"),utf8_literal("\u2068हिन्दी\u2069"),utf8_literal("\u2066אב\u2069"),
        utf8_literal("\u202c\u2069"),utf8_literal("\u202e\u202c\u2069"),utf8_literal("\u202eسلام हिन्दी\u202c"),
        utf8_literal("אָבּ גּד"),utf8_literal("אב\u200d \u200cגד "),utf8_literal("سَلَامٌ لَا"),utf8_literal("لَا لِا اَلْعَرَبِيَّةُ"),
        utf8_literal("سَّلَام 👩‍👩‍👧‍👦 A"),utf8_literal("\u202dسَلَامٌ\u202c"),utf8_literal("\u202eि क॑ कः\u202c"),utf8_literal(" A אב  \r\nسَلَامٌ  "),
        utf8_literal("👩‍👩‍👧‍👦"),utf8_literal("A 👩‍👩‍👧‍👦 B"),utf8_literal("👨‍👩‍👧‍👦"),
        utf8_literal("سلام 👨‍👩‍👧‍👦 A"),utf8_literal("אב 👩‍👩‍👧‍👦 A"),utf8_literal("👩‍👩‍👧"),utf8_literal("\u202dب\u034fب\u202c"),
        utf8_literal("\u202d\u034fبب ب\u034fب\u202c"),utf8_literal("A \u202dبَ\u034fب ب\u034fَّ\u202c 😀")};
    const Utf8View arabic_context[]{directional[35],directional[36],
        utf8_literal("\u202d  \u034fبب ب\u034fب  \u202c"),utf8_literal("\u202dب\u034fب   بَ\u034fب\u202c"),
        utf8_literal("\u202d ب\u034fب (بب) . ب\u034fب\u202c"),utf8_literal("ب\u034fب بَ\u034fب"),
        utf8_literal("\u202dب\u034fب \u200dب \u200cب\u202c"),utf8_literal("\u202d\u034fبب\r\nب\u034fب\u202c"),
        utf8_literal("\u202d بب (بب) . بب\u202c"),utf8_literal(" ب\u034fب (بب) . ب\u034fب"),
        utf8_literal("\u202dب\u034fب [بب] , ب\u034fب\u202c"),utf8_literal("\u202dبب — «بب» : بب\u202c"),
        utf8_literal(" بب (بب) . بب")};
    const ConstSlice<Utf8View> samples=tibetan_profiles?ConstSlice<Utf8View>{tibetan,sizeof(tibetan)/sizeof(tibetan[0])}:myanmar_profiles?ConstSlice<Utf8View>{myanmar,sizeof(myanmar)/sizeof(myanmar[0])}:khmer_profiles?ConstSlice<Utf8View>{khmer,sizeof(khmer)/sizeof(khmer[0])}:sinhala_profiles?ConstSlice<Utf8View>{sinhala,sizeof(sinhala)/sizeof(sinhala[0])}:malayalam_profiles?ConstSlice<Utf8View>{malayalam,sizeof(malayalam)/sizeof(malayalam[0])}:kannada_profiles?ConstSlice<Utf8View>{kannada,sizeof(kannada)/sizeof(kannada[0])}:telugu_profiles?ConstSlice<Utf8View>{telugu,sizeof(telugu)/sizeof(telugu[0])}:odia_profiles?ConstSlice<Utf8View>{odia,sizeof(odia)/sizeof(odia[0])}:gurmukhi_profiles?ConstSlice<Utf8View>{gurmukhi,sizeof(gurmukhi)/sizeof(gurmukhi[0])}:gujarati_profiles?ConstSlice<Utf8View>{gujarati,sizeof(gujarati)/sizeof(gujarati[0])}:bengali_profiles?ConstSlice<Utf8View>{bengali,sizeof(bengali)/sizeof(bengali[0])}:tamil_profiles&&devanagari_directional?ConstSlice<Utf8View>{tamil_direction,sizeof(tamil_direction)/sizeof(tamil_direction[0])}:tamil_profiles?ConstSlice<Utf8View>{tamil,sizeof(tamil)/sizeof(tamil[0])}:arabic_context_profiles?ConstSlice<Utf8View>{arabic_context,sizeof(arabic_context)/sizeof(arabic_context[0])}:devanagari_directional?ConstSlice<Utf8View>{directional,sizeof(directional)/sizeof(directional[0])}:ConstSlice<Utf8View>{ordinary,sizeof(ordinary)/sizeof(ordinary[0])};
    wchar_t selection[12]{};SetLastError(ERROR_SUCCESS);
    const DWORD selection_length=GetEnvironmentVariableW(L"MAICORD_DEVANAGARI_SAMPLE",selection,12);
    const bool selected=selection_length!=0||GetLastError()!=ERROR_ENVVAR_NOT_FOUND;u32 selected_sample=0;
    if(selected){CHECK(selection_length>0&&selection_length<12);
        for(u32 i=0;i<selection_length;++i){CHECK(selection[i]>=L'0'&&selection[i]<=L'9');
            CHECK(selected_sample<=0x19999999u);const u32 digit=static_cast<u32>(selection[i]-L'0');
            CHECK(selected_sample<0x19999999u||digit<=5);selected_sample=selected_sample*10+digit;}
        CHECK(selected_sample<samples.count);print(tibetan_profiles?"Diagnostic Tibetan sample=":myanmar_profiles?"Diagnostic Myanmar sample=":khmer_profiles?"Diagnostic Khmer sample=":sinhala_profiles?"Diagnostic Sinhala sample=":malayalam_profiles?"Diagnostic Malayalam sample=":kannada_profiles?"Diagnostic Kannada sample=":telugu_profiles?"Diagnostic Telugu sample=":odia_profiles?"Diagnostic Odia sample=":gurmukhi_profiles?"Diagnostic Gurmukhi sample=":gujarati_profiles?"Diagnostic Gujarati sample=":bengali_profiles?"Diagnostic Bengali sample=":tamil_profiles?"Diagnostic Tamil sample=":"Diagnostic Devanagari sample=");number(selected_sample);print("; separate control corpus excluded\n");}
    const u32 flags[]{0,text_style_bold,text_style_italic,text_style_underline,text_style_monospace,text_style_heading_3};const float widths[]{1000000,37};
    auto native=system,reference=system;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    for(u32 sample=0;sample<samples.count;++sample)for(u32 mode=0;mode<2;++mode)for(u32 flag:flags)for(float width:widths)
        for(u32 partial=0;partial<(sample==1&&flag?2u:1u);++partial)for(u32 tracked=0;tracked<(devanagari_tracking?(tamil_shape_only?3u:2u):1u);++tracked){
        if(selected&&sample!=selected_sample)continue;
        system.tracking=native.tracking=reference.tracking=devanagari_tracking?(tracked==2?0:tracked?.13f:.03f):0;
        ++cases;native.independent_letters=reference.independent_letters=mode!=0;const auto text=samples[sample];
        const TextStyleSpan style{0,partial?static_cast<u32>(utf8_decode_one(text.data,text.bytes).width):text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};
        const auto wrap=width==37?TextWrapping::wrap:TextWrapping::no_wrap;
        const auto expected=text_shape_utf8(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,wrap);
        const auto actual=text_shape_utf8(&native,text,styles,width,1000000,{utf16,256},&native_output.text,wrap);
        if(!expected.ok()||!actual.ok()||!native_output.text.native_fonts()||!same(native_output,reference_output,true)){
            const Output* outputs[]{&native_output,&reference_output};for(u32 side=0;side<2;++side){const auto& t=outputs[side]->text;
                for(u32 i=0;i<t.run_count;++i){const auto& r=t.runs[i];print(side?"ref shape run=":"native shape run=");number(r.text_position);print("/");number(r.text_length);print("/");number(r.glyph_count);print(" level=");number(r.bidi_level);print(" x=");real(r.baseline_x);print(" advances=");
                    for(u32 g=0;g<r.glyph_count;++g){if(g)print(",");real(t.glyphs[r.first_glyph+g].advance);}print("\n");}}
            print(tibetan_profiles?"Tibetan app sample/mode/style/partial/width/em/error=":myanmar_profiles?"Myanmar app sample/mode/style/partial/width/em/error=":khmer_profiles?"Khmer app sample/mode/style/partial/width/em/error=":sinhala_profiles?"Sinhala app sample/mode/style/partial/width/em/error=":malayalam_profiles?"Malayalam app sample/mode/style/partial/width/em/error=":kannada_profiles?"Kannada app sample/mode/style/partial/width/em/error=":telugu_profiles?"Telugu app sample/mode/style/partial/width/em/error=":odia_profiles?"Odia app sample/mode/style/partial/width/em/error=":gurmukhi_profiles?"Gurmukhi app sample/mode/style/partial/width/em/error=":gujarati_profiles?"Gujarati app sample/mode/style/partial/width/em/error=":bengali_profiles?"Bengali app sample/mode/style/partial/width/em/error=":tamil_profiles?"Tamil app sample/mode/style/partial/width/em/error=":"Devanagari app sample/mode/style/partial/width/em/error=");number(sample);print("/");number(mode);print("/");number(flag);print("/");number(partial);print("/");real(width);print("/");real(system.em_size);print("/");number(static_cast<u32>(actual.error));print("\n");return false;}
        ++native_cases;if(!tamil_shape_only&&!mode&&!compare_hits(system,text,width,wrap,styles,true,devanagari_tracking)){
            print("Directional hits sample/style/width/em/tracked=");number(sample);print("/");number(flag);print("/");real(width);print("/");real(system.em_size);print("/");number(tracked);print("\n");return false;}
    }if(!selected&&!tamil_profiles&&!arabic_context_profiles&&devanagari_directional&&system.em_size==14)CHECK(native_bidi_format_controls(system));return true;
}
