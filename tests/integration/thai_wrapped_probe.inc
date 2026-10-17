// Shared real-profile fixture; the original six Thai inputs remain unchanged.
enum class WrappedProbeScript:u8 {thai,tamil,bengali,gujarati,gurmukhi,odia,telugu,kannada,malayalam,sinhala,khmer,myanmar,tibetan};
bool probe_script_wrapped(WrappedProbeScript script) noexcept {
    const bool tibetan=script==WrappedProbeScript::tibetan,myanmar=script==WrappedProbeScript::myanmar,khmer=script==WrappedProbeScript::khmer,sinhala=script==WrappedProbeScript::sinhala,malayalam=script==WrappedProbeScript::malayalam,kannada=script==WrappedProbeScript::kannada,telugu=script==WrappedProbeScript::telugu,odia=script==WrappedProbeScript::odia,tamil=script==WrappedProbeScript::tamil,bengali=script==WrappedProbeScript::bengali,gujarati=script==WrappedProbeScript::gujarati,gurmukhi=script==WrappedProbeScript::gurmukhi;
    const char* tag=tibetan?"tibetan_wrap":myanmar?"myanmar_wrap":khmer?"khmer_wrap":sinhala?"sinhala_wrap":malayalam?"malayalam_wrap":kannada?"kannada_wrap":telugu?"telugu_wrap":odia?"odia_wrap":gurmukhi?"gurmukhi_wrap":gujarati?"gujarati_wrap":bengali?"bengali_wrap":tamil?"tamil_wrap":"thai_wrap";
    auto* data=static_cast<UserProfile*>(VirtualAlloc(nullptr,sizeof(UserProfile),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!data)return false;
    u32 scratch[512];
    const auto json=tibetan?utf8_literal(R"({"user":{"id":"4242","username":"native.test","global_name":"Native Tibetan"},"user_profile":{"bio":"Tibetan paragraph probe"}})"):myanmar?utf8_literal(R"({"user":{"id":"4242","username":"native.test","global_name":"Native Myanmar"},"user_profile":{"bio":"Myanmar paragraph probe"}})"):khmer?utf8_literal(R"({"user":{"id":"4242","username":"native.test","global_name":"Native Khmer"},"user_profile":{"bio":"Khmer paragraph probe"}})"):sinhala?utf8_literal(R"({"user":{"id":"4242","username":"native.test","global_name":"Native Sinhala"},"user_profile":{"bio":"Sinhala paragraph probe"}})"):malayalam?utf8_literal(R"({"user":{"id":"4242","username":"native.test","global_name":"Native Malayalam"},"user_profile":{"bio":"Malayalam paragraph probe"}})"):kannada?utf8_literal(R"({"user":{"id":"4242","username":"native.test","global_name":"Native Kannada"},"user_profile":{"bio":"Kannada paragraph probe"}})"):telugu?utf8_literal(R"({"user":{"id":"4242","username":"native.test","global_name":"Native Telugu"},"user_profile":{"bio":"Telugu paragraph probe"}})"):odia?utf8_literal(R"({"user":{"id":"4242","username":"native.test","global_name":"Native Odia"},"user_profile":{"bio":"Odia paragraph probe"}})"):gurmukhi?utf8_literal(R"({"user":{"id":"4242","username":"native.test","global_name":"Native Gurmukhi"},"user_profile":{"bio":"Gurmukhi paragraph probe"}})"):gujarati?utf8_literal(R"({"user":{"id":"4242","username":"native.test","global_name":"Native Gujarati"},"user_profile":{"bio":"Gujarati paragraph probe"}})"):bengali?utf8_literal(R"({"user":{"id":"4242","username":"native.test","global_name":"Native Bengali"},"user_profile":{"bio":"Bengali paragraph probe"}})"):tamil?utf8_literal(R"({"user":{"id":"4242","username":"native.test","global_name":"Native Tamil"},"user_profile":{"bio":"Tamil paragraph probe"}})"):utf8_literal(R"({"user":{"id":"4242","username":"native.test","global_name":"Native Thai"},"user_profile":{"bio":"Thai paragraph probe"}})");
    bool valid=user_profile_decode(json,{scratch,512},4242,0,data);
    log_u64(LogLevel::info,tag,"decoded",valid);
    ProfileScene scene{};scene.phase=ProfilePhase::ready;scene.measure=&g_probe.text_measure_cache;
    auto& renderer=g_probe.renderer;auto& system=renderer.glyph_text.system;
    float viewport_width=1000000;
    const auto viewport=[&]() noexcept {return UiRect{0,0,minimum(viewport_width,static_cast<float>(renderer.width)*96/renderer.dpi),static_cast<float>(renderer.height)*96/renderer.dpi};};
    valid=valid&&profile_scene_open(&scene,data,&g_probe.chat_state,&g_probe.users,1,{420,90,480,134},viewport());
    log_u64(LogLevel::info,tag,"opened",valid);
    renderer.profile_scene=&scene;
    const Utf8View tibetan_samples[]{utf8_literal("བོད་ཡིག བཀྲ་ཤིས་བདེ་ལེགས 😀 "),
        utf8_literal("ཀི ཀུ ཀཱི ཀྲྀ ཀླྀ ཀཱྀ བསྒྲུབས 😀 "),utf8_literal("ཀྐི ཀ\u200d ཀ\u200c ཀ\u034fི 😀 "),
        utf8_literal("བོད་ཡིག မြန်မာ ខ្មែរ සිංහල हिन्दी 👩‍👩‍👧‍👦 "),
        utf8_literal("A \u202eབོད་ཡིག ཀཱི\u202c B 😀 "),utf8_literal("ི ཱི ྐ ༿ ཀཷ ཀཹ ཿ 😀 ")};
    const Utf8View myanmar_samples[]{utf8_literal("မြန်မာဘာသာ မင်္ဂလာပါ ကမ္ဘာ 😀 "),
        utf8_literal("ကိ ကီ ကု ကူ ကေ ကို ကော ကျောင်း 😀 "),utf8_literal("င်္ကေ ကျွှ က\u200d က\u200c က\u034f 😀 "),
        utf8_literal("မြန်မာ ខ្មែរ සිංහල தமிழ் हिन्दी 👩‍👩‍👧‍👦 "),
        utf8_literal("A \u202eမြန်မာ ကေ ကို\u202c B 😀 "),utf8_literal("ေ ိ ု ္ ် ြ ကံ့ က့ံ 😀 ")};
    const Utf8View khmer_samples[]{utf8_literal("ភាសាខ្មែរ សួស្តី កម្ពុជា 😀 "),
        utf8_literal("កិ កី កុ កូ កេ កែ កៃ កើ កឿ កៀ កោ កៅ 😀 "),utf8_literal("ក្រ្ក ក្ក្រ ក\u200d ក\u200c ក\u034f 😀 "),
        utf8_literal("ខ្មែរ සිංහල മലയാളം தமிழ் हिन्दी 👩‍👩‍👧‍👦 "),
        utf8_literal("A \u202eភាសាខ្មែរ កើ កោ\u202c B 😀 "),utf8_literal("េ ើ ោ ៅ ្ ៌ ក\u17b4 ក\u17b5 😀 ")};
    const Utf8View sinhala_samples[]{utf8_literal("සිංහල භාෂාව ආයුබෝවන් 😀 "),
        utf8_literal("කි කී කු කූ කෙ කේ කෛ කො කෝ කෞ 😀 "),utf8_literal("ශ්‍රී ලංකාව ක්‍ෂ ක්‌ෂ ර්කි 😀 "),
        utf8_literal("සිං മല ಕನ తె ଓଡ଼ିଆ ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ் 👩‍👩‍👧‍👦 "),
        utf8_literal("A \u202eසිංහල කි කො\u202c B 😀 "),utf8_literal("කො කෝ කෞ කො කෝ කෞ ි ෙ ් ක\u034fි 😀 ")};
    const Utf8View malayalam_samples[]{utf8_literal("മലയാളം ഭാഷ നമസ്കാരം 😀 "),
        utf8_literal("കി കീ കു കൂ കൈ കൊ കോ കൗ കൊ കോ കൌ 😀 "),utf8_literal("ക്‍ഷ ക്‌ഷ ക്ഷേത്രം ശ്രദ്ധ ൎകി ർ ൻ ൽ ൾ ൺ ൿ 😀 "),
        utf8_literal("മല ಕನ తెలుగు ଓଡ଼ିଆ ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ் 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eമലയാളം കി കൈ\u202c B 😀 "),
        utf8_literal("കൊ കോ കൗ കൊ കോ കൌ ി ൈ ് ൎ ർ ൻ 😀 ")};
    const Utf8View kannada_samples[]{utf8_literal("ಕನ್ನಡ ಭಾಷೆ ನಮಸ್ಕಾರ 😀 "),
        utf8_literal("ಕಿ ಕೀ ಕು ಕೂ ಕೈ ಕೊ ಕೋ ಕೌ ರ್ಕಿ 😀 "),utf8_literal("ಕ್‍ಷ ಕ್‌ಷ ಕ್ಷೇತ್ರ ಶ್ರದ್ಧೆ ರ್ಕೋ 😀 "),
        utf8_literal("ಕನ್ನಡ తెలుగు ଓଡ଼ିଆ ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ் 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eಕನ್ನಡ ಕಿ ಕೈ\u202c B 😀 "),
        utf8_literal("ಕೃ ಕೄ ಕೊ ಕೋ ಕೌ ಕೊ ಕೋ ಿ ೈ ್ 😀 ")};
    const Utf8View telugu_samples[]{utf8_literal("తెలుగు భాష నమస్కారం 😀 "),
        utf8_literal("కి కీ కు కూ కై కొ కో కౌ క్రై 😀 "),utf8_literal("క్‍ష క్‌ష క్షేత్రం శ్రద్ధ క్ర్రై 😀 "),
        utf8_literal("తెలుగు ଓଡ଼ିଆ ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ் English 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eతెలుగు కి కై\u202c B 😀 "),
        utf8_literal("కృ కౄ కాు కైు కైంఁఁ‌ ి ై ్ 😀 ")};
    const Utf8View odia_samples[]{utf8_literal("ଓଡ଼ିଆ ଭାଷା ନମସ୍କାର 😀 "),
        utf8_literal("କି କୀ କୁ କୂ କୈ କୋ କୌ ର୍କି 😀 "),utf8_literal("କ୍‍ଷ କ୍‌ଷ କ୍ଷେତ୍ର ଶ୍ରଦ୍ଧା 😀 "),
        utf8_literal("ଓଡ଼ିଆ ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ் English 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eଓଡ଼ିଆ କି କୌ\u202c B 😀 "),
        utf8_literal("ରୁୀଁ କେ୕ୗଃ କ୕୍୕ୗ କ୍ଯୁଁ ି ୌ ୍ 😀 ")};
    const Utf8View gurmukhi_samples[]{utf8_literal("ਪੰਜਾਬੀ ਭਾਸ਼ਾ ਸਤਿ ਸ੍ਰੀ ਅਕਾਲ 😀 "),
        utf8_literal("ਕਿ ਕੀ ਕੁ ਕੂ ਕੈ ੲ ੳ 😀 "),utf8_literal("ਕ੍‍ਸ਼ ਕ੍‌ਸ਼ ਪ੍ਰਾਰਥਨਾ ਸ਼ਰਧਾ 😀 "),
        utf8_literal("ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ் English 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eਪੰਜਾਬੀ ਕਿ ਕੂ\u202c B 😀 "),
        utf8_literal("ਕੑ ਕੵ ਕ੍ਕਿ ਕੀਂ ੑ ੵ 😀 ")};
    const Utf8View gujarati_samples[]{utf8_literal("ગુજરાતી ભાષા નમસ્તે 😀 "),
        utf8_literal("કિ કો કૌ ર્કિ કૉ કૅ 😀 "),utf8_literal("ક્‍ષ ક્‌ષ ક્ષેત્ર શ્રદ્ધા 😀 "),
        utf8_literal("ગુજરાતી हिन्दी বাংলা தமிழ் English 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eગુજરાતી કો કૌ\u202c B 😀 "),
        utf8_literal("ક઼્કિ ર઼્કિ ક્\u0afd ક્ઁ\u200c 😀 ")};
    const Utf8View bengali_samples[]{utf8_literal("বাংলা ভাষা নমস্কার 😀 "),
        utf8_literal("কো কৌ কো কৌ র্কি 😀 "),utf8_literal("ক্‍ষ ক্‌ষ ক্ষেত্র শ্রদ্ধা 😀 "),
        utf8_literal("বাংলা हिन्दी தமிழ் English 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eবাংলা কো কৌ\u202c B 😀 "),
        utf8_literal("ে ৌ ্ বাংলা কি কী কু কূ 😀 ")};
    const Utf8View tamil_samples[]{utf8_literal("தமிழ் மொழி வணக்கம் 😀 "),
        utf8_literal("கொ கோ கௌ கொ கோ கௌ 😀 "),utf8_literal("க்‍க க்‌க க்ஷே ஸ்ரீ தமிழ் 😀 "),
        utf8_literal("தமிழ் हिन्दी English 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eதமிழ் கொ கோ\u202c B 😀 "),
        utf8_literal("ெ ௌ ் தமிழ் கி கீ கு கூ 😀 ")};
    const Utf8View thai_samples[]{
        utf8_literal("ภาษาไทยไม่มีช่องว่าง น้ำ 😀 "),
        utf8_literal("วันนี้อากาศดีมาก เขาเดินทางกลับบ้าน 😀 "),
        utf8_literal("การเชื่อมต่ออินเทอร์เน็ตขัดข้อง 😀 "),
        utf8_literal("ไทย น้ำ ພາສາລາວ ນ້ຳ 😀 "),
        // Compound words whose boundary decisions exercise native lookahead.
        utf8_literal("กะระตะอะเกต กรุบกริบยิม งานบริหารเหี่ย 😀 "),
        // Dictionary endpoints must retain attached marks and leading vowels.
        utf8_literal("วิญญัติ การ์ฟิลด์ดำแดง โมโหโการตรวจจับ 😀 ")};
    const ConstSlice<Utf8View> samples=tibetan?ConstSlice<Utf8View>{tibetan_samples,6}:myanmar?ConstSlice<Utf8View>{myanmar_samples,6}:khmer?ConstSlice<Utf8View>{khmer_samples,6}:sinhala?ConstSlice<Utf8View>{sinhala_samples,6}:malayalam?ConstSlice<Utf8View>{malayalam_samples,6}:kannada?ConstSlice<Utf8View>{kannada_samples,6}:telugu?ConstSlice<Utf8View>{telugu_samples,6}:odia?ConstSlice<Utf8View>{odia_samples,6}:gurmukhi?ConstSlice<Utf8View>{gurmukhi_samples,6}:gujarati?ConstSlice<Utf8View>{gujarati_samples,6}:bengali?ConstSlice<Utf8View>{bengali_samples,6}:tamil?ConstSlice<Utf8View>{tamil_samples,6}:ConstSlice<Utf8View>{thai_samples,6};
    // The profile card keeps a fixed bio width, so each mode repeats a sample up to this many bytes to force wrapping.
    constexpr u32 dpis[]{96,144};constexpr u32 bio_bytes[]{160,320,480};constexpr float viewport_widths[]{144,216,360};
    TextGlyph glyphs[1024];TextRun runs[256];u16 clusters[1024];wchar_t utf16[1024];
    ShapedText shaped{};valid=valid&&shaped_text_initialize(&shaped,{glyphs,1024},{runs,256},{clusters,1024});
    for(u32 scale=0;valid&&scale<2;++scale){
        renderer_set_dpi(&renderer,dpis[scale]);
        for(u32 sample=0;valid&&sample<samples.count;++sample)for(u32 mode=0;valid&&mode<3;++mode){
            viewport_width=viewport_widths[mode];
            data->bio.bytes=0;
            while(data->bio.bytes<bio_bytes[mode]){
                if(data->bio.bytes+samples[sample].bytes>sizeof(data->bio.data)){log_write(LogLevel::error,tag,"fixture_bio_overflow");valid=false;break;}
                copy_bytes(data->bio.data+data->bio.bytes,samples[sample].data,samples[sample].bytes);
                data->bio.bytes+=static_cast<u16>(samples[sample].bytes);
            }
            valid=valid&&profile_scene_layout(&scene,viewport(),true);
            log_u64(LogLevel::info,tag,"laid_out",valid);
            UiTextCommand commands[320]{};UiTextCommandStream stream{};
            UiTextOutput stream_output{&stream, nullptr};
            valid=valid&&ui_text_command_stream_initialize(&stream,{commands,320})&&profile_scene_text_output(scene,kDefaultTheme,&stream_output, nullptr);
            bool found=false;
            for(u32 i=0;valid&&i<stream.count;++i){
                const auto& command=commands[i];if(command.owner.handle.slot!=scene.bio_widget.slot)continue;
                found=true;auto selected=renderer_text_system(&renderer,command.face);
                valid=command.wrapping==UiTextWrapping::wrap&&command.style_flags==0&&
                    text_shape_utf8_styled(&selected,command.text,command.styles,ui_rect_width(command.rectangle),ui_rect_height(command.rectangle),
                        {utf16,1024},&shaped,TextWrapping::wrap,TextOverflow::ellipsis).ok()&&shaped.line_count>1;
                valid=valid&&shaped.native_fonts();
                log_u64(LogLevel::info,tag,"case",sample*6+mode*2+scale);
                log_u64(LogLevel::info,tag,"lines",shaped.line_count);
                log_u64(LogLevel::info,tag,"width_dips",static_cast<u32>(ui_rect_width(command.rectangle)));
                shaped_text_reset(&shaped);
            }
            valid=valid&&found;
            const auto before=native_text_candidate_counts(system);
            valid=valid&&rebuild_probe_content_text()&&probe_prepare_capture();
            valid=valid&&native_text_candidate_counts(system).paragraphs_shaped>before.paragraphs_shaped;
            wchar_t tibetan_capture[]=L"tibetan-wrap-0-0-0.bmp",myanmar_capture[]=L"myanmar-wrap-0-0-0.bmp",khmer_capture[]=L"khmer-wrap-0-0-0.bmp",sinhala_capture[]=L"sinhala-wrap-0-0-0.bmp",malayalam_capture[]=L"malayalam-wrap-0-0-0.bmp",kannada_capture[]=L"kannada-wrap-0-0-0.bmp",telugu_capture[]=L"telugu-wrap-0-0-0.bmp",odia_capture[]=L"odia-wrap-0-0-0.bmp",thai_capture[]=L"thai-wrap-0-0-0.bmp",tamil_capture[]=L"tamil-wrap-0-0-0.bmp",bengali_capture[]=L"bengali-wrap-0-0-0.bmp",gujarati_capture[]=L"gujarati-wrap-0-0-0.bmp",gurmukhi_capture[]=L"gurmukhi-wrap-0-0-0.bmp";
            auto* capture=tibetan?tibetan_capture:myanmar?myanmar_capture:khmer?khmer_capture:sinhala?sinhala_capture:malayalam?malayalam_capture:kannada?kannada_capture:telugu?telugu_capture:odia?odia_capture:gurmukhi?gurmukhi_capture:gujarati?gujarati_capture:bengali?bengali_capture:tamil?tamil_capture:thai_capture;const u32 digit=tibetan?13u:myanmar?13u:khmer?11u:sinhala?13u:malayalam?15u:kannada?13u:telugu?12u:odia?10u:gurmukhi?14u:gujarati?14u:bengali?13u:tamil?11u:10u;
            capture[digit]+=static_cast<wchar_t>(sample);capture[digit+2]+=static_cast<wchar_t>(mode);capture[digit+4]+=static_cast<wchar_t>(scale);
            valid=valid&&probe_capture_frame(capture);
        }
    }
    shaped_text_reset(&shaped);renderer.profile_scene=nullptr;profile_scene_release(&scene);VirtualFree(data,0,MEM_RELEASE);
    return valid;
}
