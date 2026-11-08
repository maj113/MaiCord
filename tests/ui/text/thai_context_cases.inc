// Thai break opportunities from the native word breaker: maximal matching
// over the open ICU and PyThaiNLP lexicon (tools/unicode/thai_break_data.py)
// and the first-party syllable grammar for unknown text
// (tools/unicode/thai_syllable_data.py). '|' marks each expected opportunity
// after cluster projection. The cases cover ambiguous word sequences, unknown
// syllables and finals, silent letters, attached and orphan marks, MAIYAMOK
// and wrapped corpus samples.
// Primitive contracts of the word breaker. Candidate lengths are the lexicon
// words starting at a position, longest first; syllable results and recovery
// lengths are one more than the scalars consumed (negative: open syllable).
bool native_thai_primitive_cases() noexcept {
    u32 scalars[32]{};
    const auto decode=[&](const char* value) noexcept {
        u32 count=0;const u32 bytes=c_string_length(value);
        for(u32 at=0;at<bytes&&count<32;){const auto d=utf8_decode_one(value+at,bytes-at);if(!d.valid)return 0u;scalars[count++]=d.codepoint;at+=d.width;}
        return count;
    };
    struct LexicalCase {const char* text;u32 position;i32 lengths[4];};
    const LexicalCase lexical[]{
        {"ภาษาไทยไม่มีช่องว่าง",0,{4,2}},{"ภาษาไทยไม่มีช่องว่าง",4,{3,2}},{"ภาษาไทยไม่มีช่องว่าง",7,{5,3}},
        {"ภาษาไทยไม่มีช่องว่าง",9,{}},{"ภาษาไทยไม่มีช่องว่าง",12,{4,3}},
        {"จันทร์เจ้า",0,{6}},     // not จัน before the silent ทร์
        {"ต่างๆ",0,{4}},{"ต่างๆ",4,{1}},{"ตากลม",0,{5,3,2}},{"มากกว่า",0,{7,3,2}},
        {"ไม่ิ",0,{}},            // not before an attached mark
        {"กระทรวงการต่างประเทศ",0,{7,3}},  // PyThaiNLP compounds are left out
    };
    for(const auto& sample:lexical){
        ++cases;const u32 count=decode(sample.text);CHECK(count);
        const auto actual=native_thai_lexical_candidates({scalars,count},sample.position);
        for(u32 i=0;i<8;++i)if(actual.lengths[i]!=(i<4?sample.lengths[i]:0)){
            print("Thai lexical case/slot/actual=");number(cases);print("/");number(i);print("/");number(static_cast<u32>(actual.lengths[i]));print("\n");return false;}
    }
    struct SyllableCase {const char* text;i32 result;u32 recovery;};
    const SyllableCase syllables[]{
        {"กะ",3,3},{"กาน",-3,4},{"กานิ",-3,3},{"กัน",4,4},{"ตั๋ว",5,5},{"คือ",4,4},{"ค่ำ",4,4},{"คน",3,3},
        {"ปลา",-4,4},{"เกาะ",5,5},{"เรียนรู้",-5,6},{"เกิด",5,5},{"เป็น",5,5},{"แกง",4,4},{"ไม่",4,4},
        {"ควร",4,4},{"สวัสดี",5,5},{"จันทร์",4,7},{"กำ๊",3,4},{"ฤๅ",3,3},{"ิก",0,0},{"เ",0,0},
    };
    for(const auto& sample:syllables){
        ++cases;const u32 count=decode(sample.text);CHECK(count);
        const i32 result=native_thai_syllable_result({scalars,count},0);const u32 recovery=native_thai_unknown_recovery({scalars,count},0,result);
        if(result!=sample.result||recovery!=sample.recovery){
            print("Thai syllable case/result/recovery=");number(cases);print("/");number(static_cast<u32>(result));print("/");number(recovery);print("\n");return false;}
    }
    CHECK(native_thai_syllable_result({},0)==0&&native_thai_unknown_recovery({},0,0)==0);
    CHECK(native_thai_lexical_candidates({scalars,2},2).lengths[0]==0);
    const u32 attached[]{0xe30,0xe31,0xe32,0xe33,0xe34,0xe3a,0xe45,0xe47,0xe48,0xe4c,0xe4e};
    const u32 begins[]{0,0x20,0xe01,0xe2e,0xe2f,0xe3b,0xe3f,0xe40,0xe44,0xe46,0xe4f,0xe5b,0xe80};
    for(u32 cp:attached)CHECK(!native_thai_boundary_class(cp));
    for(u32 cp:begins)CHECK(native_thai_boundary_class(cp));
    // A run longer than one search window divides as its repeated sentence does.
    const u32 sentence=decode("ภาษาไทยไม่มีช่องว่าง");CHECK(sentence==20);
    u32 run[400]{};u8 run_breaks[401]{};for(u32 i=0;i<400;++i)run[i]=scalars[i%sentence];
    CHECK(native_thai_word_breaks({run,400},{run_breaks,401}));
    for(u32 i=1;i<400;++i){const u32 at=i%sentence;CHECK((run_breaks[i]!=0)==(at==0||at==4||at==7||at==12||at==16));}
    native_cases+=sizeof(lexical)/sizeof(lexical[0])+sizeof(syllables)/sizeof(syllables[0]);
    return true;
}
bool native_thai_context_cases() noexcept {
    if(!native_thai_primitive_cases())return false;
    // Expected segmentations: the engine's Thai dictionary breaks, projected onto its shaped clusters.
    const char* const samples[]{
        "กรม|กรอม|แล้ว|อยาก|อึ๊บ",
        "กรุบกริบ|ยิม",
        "กะระตะ|อะเกต",
        "กุล|ทู|สก|ฝรั่น|ของ|ระบาด|ปังตอ",
        "งาน|บริหาร|เหี่ย",
        "ดอก|ผล|ธรรมดา|แอ๊คชั่น|อากัป|กริยา|ปะ|ติ|ยาน",
        "ตยาค|ดอก|ผล|ธรรมดา|แบ็กอัป",
        "ตะบอย|ทะท่าว|วิลาวัณย์|มัชชาร์",
        "ท้อแท้|เก|เส|หาง|กระ|หมวด|ขรุขระ|ส้นตีน|ส้น|มือ",
        "นิเวศวิทยา|ขวักไขว่|ถ้อยคำ|ทุพพลภาพ",
        "มนุ|พลี|รังสิมา|เร็กคอร์ด",
        "มายา|คติ|อะลุ่มอล่วย",
        "ระ|บา|นี้|เฟอร์เบอไรต์",
        "ลำโพง|ปืน|ต่อสู้|อากาศยาน|เฮิรต์|โกรญจ",
        "วิทยาคม|เดียน",
        "สิโรดม|ควิโต|พ่อตา|ขยับเขยื้อน|เปล",
        "อัมพุท|กรร|ทบ|เวสปอยต์",
        "อายุขัย|ว|เอดจ์",
        "อุทุมพร|คนี",
        "เพสลาด|ไบรท์|ไม่มี",
        "เส็ก|อมาวาสี|สุจริต|ใจ|มูก|หลวง|เปอร์โตริโก",
        "เอกมัย|แนวที่ห้า|อาตมภาพ|เรียลลิสต์|สมใจ",
        "เอกรูป|มูฟวี่",
        "ไพรจิตร|ตึ้ก",
        "วิญญัติ",
        "เซปักตระกร้อ|ไฮโดรเจนซัลไฟด์|วิญญัติ",
        "บูรณะ|สัตวาร|หลับใน|วิญญัติ",
        "ยะหิทา|กล่อง|เพลง|กะโบม|ทรงมัณฑ์",
        "การ์ฟิลด์|ดำแดง",
        "ยาจนก|เทพ|บริษัท|โมโห|โการ|ตรวจ|จับ",
        "สยามินทร์|ลำเอียก|จีน|กลาง|ลุก|เป็น|ไฟ|การ|ติด|โรค",
        "หรบๆัภาษา",
        "หรบๆำภาษา",
        "หรบๆฺภาษา",
        "หรบๆ์ภาษา",
        // Wrapped corpus samples 6, 21, 22 and 23 (thai_cases.inc).
        "ภาษา|ไทย|ไม่มี|ช่อง|ว่าง",
        "ลัก|ขณะฺ|ภาษา |กฅ|ฆะฺ|ภาษา |กกะฺ |กกะฺ์ |กกะฺก์",
        "ฟำ๎|ฬ|ม๊|ฒิผ",
        "กำั |กำิ |กำี |กำึ |กำื |กำุ |กำู |กำฺ |กำ็ |กำ่ |กำ้ |กำ๊ |กำ๋ |กำ์ |กำํ |กำ๎ |กำำ |ะฺ |ำฺ |าฺ |เฺ",
    };
    u32 failures=0;
    for(const char* segmented:samples){
        ++cases;u32 count=0;const u32 bytes=c_string_length(segmented);
        u32 scalars[256]{};u8 expected[257]{};
        for(u32 at=0;at<bytes;){
            if(segmented[at]=='|'){expected[count]=1;++at;continue;}
            const auto decoded=utf8_decode_one(segmented+at,bytes-at);
            CHECK(decoded.valid&&decoded.codepoint<0x10000&&count<255);
            scalars[count++]=decoded.codepoint;at+=decoded.width;
        }
        u8 native[257]{};CHECK(unicode_line_breaks({scalars,count},{native,257}));
        CHECK(native_thai_word_breaks({scalars,count},{native,257}));
        // Cluster metrics expose opportunities only at shaped-cluster ends.
        // Project through the actual native provider, not a scalar-pair guess.
        CHECK(thai_project_native_clusters({scalars,count},{native,257}));
        bool equal=true;for(u32 i=1;i<count;++i)equal=equal&&(native[i]!=0)==(expected[i]!=0);
        if(!equal){
            char actual[1024];u32 size=0;
            for(u32 i=0;i<count;++i){if(i&&native[i])actual[size++]='|';size+=utf8_encode(scalars[i],actual+size);}
            actual[size]=0;print("Thai break case/actual=");number(cases);print("/");print(actual);print("\n");++failures;continue;
        }
        ++native_cases;
    }
    print("thai_break_cases=");number(sizeof(samples)/sizeof(samples[0]));print("\n");
    return failures==0;
}
