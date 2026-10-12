bool native_thai_corpus(DWriteTextSystem system) noexcept {
    const Utf8View samples[]{utf8_literal("ภาษาไทย"),utf8_literal("สวัสดีครับ"),utf8_literal("น้ำ กำ ก่ำ"),utf8_literal("ปิ ปี่ ปุ ญู ฐุ"),
        utf8_literal("กิ่ ก่ิ กิิ กุู"),utf8_literal("่ ิ ุ ํ ำ"),utf8_literal("ภาษาไทยไม่มีช่องว่าง"),
        utf8_literal("ພາສາລາວ"),utf8_literal("ສະບາຍດີ"),utf8_literal("ນ້ຳ ກຳ ກ່ຳ"),utf8_literal("ປິ ປີ່ ກຸ ຍູ"),
        utf8_literal("່ ິ ຸ ໍ ຳ"),utf8_literal("A ไทย 😀 ລາວ B"),utf8_literal("ไทย\r\nລາວ"),utf8_literal("ก‍ิ ก‌ิ"),utf8_literal("aิ  ิ ‍ิ"),utf8_literal("กฺุ กฺุ"),
        utf8_literal("ก่ํ กํ่ ก่ำ กิำ"),utf8_literal("เ่ าิ ๑ุ"),utf8_literal(" ່ ກ່ຼ ກິິ"),utf8_literal("ນ່ໍ ນໍ່ ນ່ຳ"),
        utf8_literal("ลักขณะฺภาษา กฅฆะฺภาษา กกะฺ กกะฺ์ กกะฺก์"),utf8_literal("ฟำ๎ฬม๊ฒิผ"),
        utf8_literal("กำั กำิ กำี กำึ กำื กำุ กำู กำฺ กำ็ กำ่ กำ้ กำ๊ กำ๋ กำ์ กำํ กำ๎ กำำ ะฺ ำฺ าฺ เฺ"),
        utf8_literal("กรีดก\u200b"),utf8_literal("\u200bภาษา\u200bไทย\u200b\u200b"),
        utf8_literal("A\u200bไทย\u200bລາວ\u200bB"),utf8_literal("\u200b\u200b"),
        utf8_literal("\u0e00"),utf8_literal("ไทย\u0e3bก\u0e7f"),utf8_literal("A\u0378B\U0003ffff"),
        utf8_literal("ເຝົ້າ ເຟົ້າ ເໜົ້າ"),utf8_literal("ໜົ້ ໝົ້ ໞົ້ ໟົ້"),utf8_literal("ໜຼ ໝຼ ໞຼ ໟຼ")};
    const u32 flags[]{0,text_style_bold,text_style_underline,text_style_monospace};const float widths[]{1000000,37};
    auto native=system,reference=system;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    for(u32 sample=0;sample<sizeof(samples)/sizeof(samples[0]);++sample)for(u32 partial=0;partial<((sample==2||sample==9||sample==17)?2u:1u);++partial)for(u32 mode=0;mode<2;++mode)for(u32 flag:flags)for(float width:widths){
        ++cases;native.independent_letters=reference.independent_letters=mode!=0;const auto text=samples[sample];const TextStyleSpan style{0,partial?static_cast<u32>(utf8_decode_one(text.data,text.bytes).width):text.bytes,flag};
        const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};
        const auto wrapping=thai_shape_only?TextWrapping::no_wrap:TextWrapping::wrap;
        const auto expected=text_shape_utf8_styled(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,wrapping);
        const auto actual=text_shape_utf8_styled(&native,text,styles,width,1000000,{utf16,256},&native_output.text,wrapping);
        if(!expected.ok()||!actual.ok()||!native_output.text.native_fonts()||!same(native_output,reference_output,true)){
            for(u32 side=0;side<2;++side){const auto& o=side?reference_output.text:native_output.text;print(side?"reference glyphs=":"native glyphs=");for(u32 g=0;g<o.glyph_count;++g){number(o.glyphs[g].index);print(" ");}print("\n");}
            print("Thai/Lao sample/mode/flag/width/em/error=");number(sample);print("/");number(mode);print("/");number(flag);print("/");real(width);print("/");real(system.em_size);print("/");number(static_cast<u32>(actual.error));print("\n");return false;}
        ++native_cases;if(!mode)CHECK(compare_hits(system,text,width,wrapping,styles,true));
    }
    return true;
}
