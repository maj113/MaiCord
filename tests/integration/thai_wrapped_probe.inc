// Shared real-profile fixture; the original six Thai inputs remain unchanged.
enum class WrappedProbeScript:u8 {thai,tamil,bengali,gujarati,gurmukhi,odia,telugu,kannada,malayalam,sinhala,khmer,myanmar,tibetan};
// One row per WrappedProbeScript in enum order: lower-case name, display title and six profile bios.
struct WrappedScriptFixture {const char* name;const char* title;Utf8View samples[6];};
const WrappedScriptFixture kWrappedScripts[]{
    {"thai","Thai",{
        utf8_literal("ภาษาไทยไม่มีช่องว่าง น้ำ 😀 "),
        utf8_literal("วันนี้อากาศดีมาก เขาเดินทางกลับบ้าน 😀 "),
        utf8_literal("การเชื่อมต่ออินเทอร์เน็ตขัดข้อง 😀 "),
        utf8_literal("ไทย น้ำ ພາສາລາວ ນ້ຳ 😀 "),
        // Compound words whose boundary decisions exercise native lookahead.
        utf8_literal("กะระตะอะเกต กรุบกริบยิม งานบริหารเหี่ย 😀 "),
        // Dictionary endpoints must retain attached marks and leading vowels.
        utf8_literal("วิญญัติ การ์ฟิลด์ดำแดง โมโหโการตรวจจับ 😀 ")}},
    {"tamil","Tamil",{utf8_literal("தமிழ் மொழி வணக்கம் 😀 "),
        utf8_literal("கொ கோ கௌ கொ கோ கௌ 😀 "),utf8_literal("க்‍க க்‌க க்ஷே ஸ்ரீ தமிழ் 😀 "),
        utf8_literal("தமிழ் हिन्दी English 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eதமிழ் கொ கோ\u202c B 😀 "),
        utf8_literal("ெ ௌ ் தமிழ் கி கீ கு கூ 😀 ")}},
    {"bengali","Bengali",{utf8_literal("বাংলা ভাষা নমস্কার 😀 "),
        utf8_literal("কো কৌ কো কৌ র্কি 😀 "),utf8_literal("ক্‍ষ ক্‌ষ ক্ষেত্র শ্রদ্ধা 😀 "),
        utf8_literal("বাংলা हिन्दी தமிழ் English 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eবাংলা কো কৌ\u202c B 😀 "),
        utf8_literal("ে ৌ ্ বাংলা কি কী কু কূ 😀 ")}},
    {"gujarati","Gujarati",{utf8_literal("ગુજરાતી ભાષા નમસ્તે 😀 "),
        utf8_literal("કિ કો કૌ ર્કિ કૉ કૅ 😀 "),utf8_literal("ક્‍ષ ક્‌ષ ક્ષેત્ર શ્રદ્ધા 😀 "),
        utf8_literal("ગુજરાતી हिन्दी বাংলা தமிழ் English 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eગુજરાતી કો કૌ\u202c B 😀 "),
        utf8_literal("ક઼્કિ ર઼્કિ ક્\u0afd ક્ઁ\u200c 😀 ")}},
    {"gurmukhi","Gurmukhi",{utf8_literal("ਪੰਜਾਬੀ ਭਾਸ਼ਾ ਸਤਿ ਸ੍ਰੀ ਅਕਾਲ 😀 "),
        utf8_literal("ਕਿ ਕੀ ਕੁ ਕੂ ਕੈ ੲ ੳ 😀 "),utf8_literal("ਕ੍‍ਸ਼ ਕ੍‌ਸ਼ ਪ੍ਰਾਰਥਨਾ ਸ਼ਰਧਾ 😀 "),
        utf8_literal("ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ் English 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eਪੰਜਾਬੀ ਕਿ ਕੂ\u202c B 😀 "),
        utf8_literal("ਕੑ ਕੵ ਕ੍ਕਿ ਕੀਂ ੑ ੵ 😀 ")}},
    {"odia","Odia",{utf8_literal("ଓଡ଼ିଆ ଭାଷା ନମସ୍କାର 😀 "),
        utf8_literal("କି କୀ କୁ କୂ କୈ କୋ କୌ ର୍କି 😀 "),utf8_literal("କ୍‍ଷ କ୍‌ଷ କ୍ଷେତ୍ର ଶ୍ରଦ୍ଧା 😀 "),
        utf8_literal("ଓଡ଼ିଆ ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ் English 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eଓଡ଼ିଆ କି କୌ\u202c B 😀 "),
        utf8_literal("ରୁୀଁ କେ୕ୗଃ କ୕୍୕ୗ କ୍ଯୁଁ ି ୌ ୍ 😀 ")}},
    {"telugu","Telugu",{utf8_literal("తెలుగు భాష నమస్కారం 😀 "),
        utf8_literal("కి కీ కు కూ కై కొ కో కౌ క్రై 😀 "),utf8_literal("క్‍ష క్‌ష క్షేత్రం శ్రద్ధ క్ర్రై 😀 "),
        utf8_literal("తెలుగు ଓଡ଼ିଆ ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ் English 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eతెలుగు కి కై\u202c B 😀 "),
        utf8_literal("కృ కౄ కాు కైు కైంఁఁ‌ ి ై ్ 😀 ")}},
    {"kannada","Kannada",{utf8_literal("ಕನ್ನಡ ಭಾಷೆ ನಮಸ್ಕಾರ 😀 "),
        utf8_literal("ಕಿ ಕೀ ಕು ಕೂ ಕೈ ಕೊ ಕೋ ಕೌ ರ್ಕಿ 😀 "),utf8_literal("ಕ್‍ಷ ಕ್‌ಷ ಕ್ಷೇತ್ರ ಶ್ರದ್ಧೆ ರ್ಕೋ 😀 "),
        utf8_literal("ಕನ್ನಡ తెలుగు ଓଡ଼ିଆ ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ் 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eಕನ್ನಡ ಕಿ ಕೈ\u202c B 😀 "),
        utf8_literal("ಕೃ ಕೄ ಕೊ ಕೋ ಕೌ ಕೊ ಕೋ ಿ ೈ ್ 😀 ")}},
    {"malayalam","Malayalam",{utf8_literal("മലയാളം ഭാഷ നമസ്കാരം 😀 "),
        utf8_literal("കി കീ കു കൂ കൈ കൊ കോ കൗ കൊ കോ കൌ 😀 "),utf8_literal("ക്‍ഷ ക്‌ഷ ക്ഷേത്രം ശ്രദ്ധ ൎകി ർ ൻ ൽ ൾ ൺ ൿ 😀 "),
        utf8_literal("മല ಕನ తెలుగు ଓଡ଼ିଆ ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ் 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eമലയാളം കി കൈ\u202c B 😀 "),
        utf8_literal("കൊ കോ കൗ കൊ കോ കൌ ി ൈ ് ൎ ർ ൻ 😀 ")}},
    {"sinhala","Sinhala",{utf8_literal("සිංහල භාෂාව ආයුබෝවන් 😀 "),
        utf8_literal("කි කී කු කූ කෙ කේ කෛ කො කෝ කෞ 😀 "),utf8_literal("ශ්‍රී ලංකාව ක්‍ෂ ක්‌ෂ ර්කි 😀 "),
        utf8_literal("සිං മല ಕನ తె ଓଡ଼ିଆ ਪੰਜਾਬੀ ગુજરાતી हिन्दी বাংলা தமிழ் 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eසිංහල කි කො\u202c B 😀 "),
        utf8_literal("කො කෝ කෞ කො කෝ කෞ ි ෙ ් ක\u034fි 😀 ")}},
    {"khmer","Khmer",{utf8_literal("ភាសាខ្មែរ សួស្តី កម្ពុជា 😀 "),
        utf8_literal("កិ កី កុ កូ កេ កែ កៃ កើ កឿ កៀ កោ កៅ 😀 "),utf8_literal("ក្រ្ក ក្ក្រ ក\u200d ក\u200c ក\u034f 😀 "),
        utf8_literal("ខ្មែរ සිංහල മലയാളം தமிழ் हिन्दी 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eភាសាខ្មែរ កើ កោ\u202c B 😀 "),
        utf8_literal("េ ើ ោ ៅ ្ ៌ ក\u17b4 ក\u17b5 😀 ")}},
    {"myanmar","Myanmar",{utf8_literal("မြန်မာဘာသာ မင်္ဂလာပါ ကမ္ဘာ 😀 "),
        utf8_literal("ကိ ကီ ကု ကူ ကေ ကို ကော ကျောင်း 😀 "),utf8_literal("င်္ကေ ကျွှ က\u200d က\u200c က\u034f 😀 "),
        utf8_literal("မြန်မာ ខ្មែរ සිංහල தமிழ் हिन्दी 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eမြန်မာ ကေ ကို\u202c B 😀 "),
        utf8_literal("ေ ိ ု ္ ် ြ ကံ့ က့ံ 😀 ")}},
    {"tibetan","Tibetan",{utf8_literal("བོད་ཡིག བཀྲ་ཤིས་བདེ་ལེགས 😀 "),
        utf8_literal("ཀི ཀུ ཀཱི ཀྲྀ ཀླྀ ཀཱྀ བསྒྲུབས 😀 "),utf8_literal("ཀྐི ཀ\u200d ཀ\u200c ཀ\u034fི 😀 "),
        utf8_literal("བོད་ཡིག မြန်မာ ខ្មែរ සිංහල हिन्दी 👩‍👩‍👧‍👦 "),utf8_literal("A \u202eབོད་ཡིག ཀཱི\u202c B 😀 "),
        utf8_literal("ི ཱི ྐ ༿ ཀཷ ཀཹ ཿ 😀 ")}},
};
bool probe_script_wrapped(WrappedProbeScript script) noexcept {
    const auto& fixture=kWrappedScripts[static_cast<u32>(script)];
    char tag[32]{},json_text[192]{};u32 tag_bytes=0,json_bytes=0;
    const auto append=[](char* output,u32* used,const char* text) noexcept {for(;*text;++text)output[(*used)++]=*text;};
    append(tag,&tag_bytes,fixture.name);append(tag,&tag_bytes,"_wrap");
    append(json_text,&json_bytes,R"({"user":{"id":"4242","username":"native.test","global_name":"Native )");
    append(json_text,&json_bytes,fixture.title);
    append(json_text,&json_bytes,R"("},"user_profile":{"bio":")");
    append(json_text,&json_bytes,fixture.title);
    append(json_text,&json_bytes,R"( paragraph probe"}})");
    const Utf8View json{json_text,json_bytes};
    auto* data=static_cast<UserProfile*>(VirtualAlloc(nullptr,sizeof(UserProfile),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!data)return false;
    u32 scratch[512];
    bool valid=user_profile_decode(json,{scratch,512},4242,0,data);
    log_u64(LogLevel::info,tag,"decoded",valid);
    ProfileScene scene{};scene.phase=ProfilePhase::ready;scene.measure=&g_probe.text_measure_cache;
    auto& renderer=g_probe.renderer;auto& system=renderer.glyph_text.system;
    float viewport_width=1000000;
    const auto viewport=[&]() noexcept {return UiRect{0,0,minimum(viewport_width,static_cast<float>(renderer.width)*96/renderer.dpi),static_cast<float>(renderer.height)*96/renderer.dpi};};
    valid=valid&&profile_scene_open(&scene,data,&g_probe.chat_state,&g_probe.users,1,{420,90,480,134},viewport());
    log_u64(LogLevel::info,tag,"opened",valid);
    renderer.profile_scene=&scene;
    const ConstSlice<Utf8View> samples{fixture.samples,6};
    // The capture name is "<script>-wrap-<sample>-<mode>-<scale>.bmp"; the three counters start at `digit`.
    u32 name_bytes=0;while(fixture.name[name_bytes])++name_bytes;
    wchar_t capture_base[48]{};
    for(u32 i=0;i<name_bytes;++i)capture_base[i]=static_cast<wchar_t>(fixture.name[i]);
    copy_bytes(capture_base+name_bytes,L"-wrap-0-0-0.bmp",sizeof(L"-wrap-0-0-0.bmp"));
    const u32 digit=name_bytes+6;
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
            wchar_t capture[48];copy_bytes(capture,capture_base,sizeof(capture));
            capture[digit]+=static_cast<wchar_t>(sample);capture[digit+2]+=static_cast<wchar_t>(mode);capture[digit+4]+=static_cast<wchar_t>(scale);
            valid=valid&&probe_capture_frame(capture);
        }
    }
    shaped_text_reset(&shaped);renderer.profile_scene=nullptr;profile_scene_release(&scene);VirtualFree(data,0,MEM_RELEASE);
    return valid;
}
