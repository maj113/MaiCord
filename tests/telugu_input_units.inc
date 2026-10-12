#include "generated/telugu_input_fixture.inc"
bool telugu_input_units() noexcept {
    const auto telugu_input_fixture=indic_input_fixture(telugu_input_fixture_resource);
    CHECK(telugu_input_fixture.count==telugu_input_fixture_words);
    constexpr auto profile=SfntIndicProfile::telugu;
    u32 scalars[256]{};u8 boundaries[256]{};u32 first=0,count=0,cases=0;
    for(u32 i=0;i<telugu_input_fixture.count;++i){const u32 entry=telugu_input_fixture[i];CHECK(count<256);scalars[count++]=entry&0x1fffffu;
        if(entry&0x40000000u){CHECK(sfnt_indic_cluster_boundaries(profile,{scalars,count},{boundaries,count},1000000,{telugu_input_forms,128})==SfntStatus::ok);
            for(u32 j=0;j<count;++j)if(boundaries[j]!=(telugu_input_fixture[first+j]>>31)){print("Telugu fixture case ");number(cases);print("\n");CHECK(false);}
            ++cases;count=0;first=i+1;}}
    CHECK(!count&&cases==telugu_input_fixture_cases);
    const u32 font_sensitive[]{0xc0b,0xc4d,0xc15,0xc3e};u8 test_forms[128]{};
    const u8 full_form[]{1,0,0,0},below_form[]{1,0,0,1};
    CHECK(sfnt_indic_cluster_boundaries(profile,{font_sensitive,4},{boundaries,4},16,{test_forms,128})==SfntStatus::ok&&bytes_equal(boundaries,full_form,4));
    test_forms[0x15]=sfnt_deva_below;
    CHECK(sfnt_indic_cluster_boundaries(profile,{font_sensitive,4},{boundaries,4},16,{test_forms,128})==SfntStatus::ok&&bytes_equal(boundaries,below_form,4));
    for(auto& b:boundaries)b=255;
    CHECK(sfnt_indic_cluster_boundaries(profile,{font_sensitive,4},{boundaries,4})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{font_sensitive,4},{boundaries,4},16,{test_forms,127})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{font_sensitive,4},{boundaries,4},16,{nullptr,128})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{font_sensitive,4},{boundaries,3},16,{test_forms,128})==SfntStatus::output_too_small);
    CHECK(sfnt_indic_cluster_boundaries(profile,{font_sensitive,4},{boundaries,4},15,{test_forms,128})==SfntStatus::resource_limit);
    CHECK(sfnt_indic_cluster_boundaries(profile,{nullptr,4},{boundaries,4},16,{test_forms,128})==SfntStatus::invalid_input);
    const u32 invalid_cluster[]{0xc15,0xd800};
    CHECK(sfnt_indic_cluster_boundaries(profile,{invalid_cluster,2},{boundaries,4},16,{test_forms,128})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{font_sensitive,4},{boundaries,4},16,{boundaries,128})==SfntStatus::invalid_input);
    for(auto b:boundaries)CHECK(b==255);
    static u32 long_input[8192];static u8 long_boundaries[8192];
    for(u32 i=0;i<8192;++i)long_input[i]=i%31?0xc41:0xc15;
    CHECK(sfnt_indic_cluster_boundaries(profile,{long_input,8192},{long_boundaries,8192},32768,{test_forms,128})==SfntStatus::ok);
    for(u32 i=0;i<8192;++i)CHECK(long_boundaries[i]==(i%31==0));
    for(u32 i=0;i<8192;++i)long_input[i]=i%2?0xc4d:0xc15;
    CHECK(sfnt_indic_cluster_boundaries(profile,{long_input,8192},{long_boundaries,8192},32768,{test_forms,128})==SfntStatus::ok);
    for(u32 i=0;i<8192;++i)CHECK(long_boundaries[i]==(i%10==0));
    SfntIndicInputScalar expanded[130];SfntIndicInputResult result{};
    u32 block[128]{};for(u32 i=0;i<128;++i)block[i]=0xc00+i;
    CHECK(sfnt_indic_expand_cluster(profile,{block,128},{expanded,130},&result)==SfntStatus::ok&&result.scalar_count==129&&result.source_units==128);
    u32 at=0;for(u32 i=0;i<128;++i){if(i==0x48){CHECK(expanded[at].scalar==0xc46&&expanded[at].source==i);++at;CHECK(expanded[at].scalar==0xc56&&expanded[at].source==i);++at;}
        else {CHECK(expanded[at].scalar==block[i]&&expanded[at].source==i);++at;}}
    CHECK(at==129);
    const u32 mixed[]{0x1f600,0xc15,0xc48,0x200d,0xc48,0x200c,0xc10,0xc4b};
    const SfntIndicInputScalar expected[]{{0x1f600,0},{0xc15,2},{0xc46,3},{0xc56,3},{0x200d,4},{0xc46,5},{0xc56,5},{0x200c,6},{0xc10,7},{0xc4b,8}};
    CHECK(sfnt_indic_expand_cluster(profile,{mixed,8},{expanded,130},&result)==SfntStatus::ok&&result.scalar_count==10&&result.source_units==9&&bytes_equal(expanded,expected,sizeof(expected)));
    const SfntIndicInputScalar limited[]{{0x1f600,0},{0xc15,2},{0xc46,3},{0xc56,3},{0x200d,4},{0xc48,5},{0x200c,6},{0xc10,7},{0xc4b,8}};
    CHECK(sfnt_indic_expand_cluster(profile,{mixed,8},{expanded,130},&result,24,1)==SfntStatus::ok&&result.scalar_count==9&&result.source_units==9&&bytes_equal(expanded,limited,sizeof(limited)));
    CHECK(sfnt_indic_expand_cluster(profile,{mixed,8},{expanded,130},&result,24,0)==SfntStatus::ok&&result.scalar_count==8&&result.source_units==9);
    u32 source=0;for(u32 i=0;i<8;++i){CHECK(expanded[i].scalar==mixed[i]&&expanded[i].source==source);source+=mixed[i]>0xffff?2:1;}
    for(u32 capacity=0;capacity<10;++capacity){for(auto& value:expanded)value={0xffffffffu,0xffffffffu};
        CHECK(sfnt_indic_expand_cluster(profile,{mixed,8},{expanded,capacity},&result)==SfntStatus::output_too_small&&result.scalar_count==10&&result.source_units==9);
        for(const auto& value:expanded)CHECK(value.scalar==0xffffffffu&&value.source==0xffffffffu);}
    for(u32 budget=0;budget<24;++budget){
        CHECK(sfnt_indic_expand_cluster(profile,{mixed,8},{expanded,130},&result,budget)==SfntStatus::resource_limit&&!result.scalar_count&&!result.source_units);
        for(const auto& value:expanded)CHECK(value.scalar==0xffffffffu&&value.source==0xffffffffu);}
    const u32 invalid[]{0xc48,0xd800};
    CHECK(sfnt_indic_expand_cluster(profile,{invalid,2},{expanded,130},&result)==SfntStatus::invalid_input&&!result.scalar_count&&!result.source_units);
    for(const auto& value:expanded)CHECK(value.scalar==0xffffffffu&&value.source==0xffffffffu);
    const u32 adjacent[]{0xb48,0xb4b,0xb4c,0xbca,0x9cb,0xcc8};
    CHECK(sfnt_indic_expand_cluster(profile,{adjacent,6},{expanded,130},&result)==SfntStatus::ok&&result.scalar_count==6&&result.source_units==6);
    for(u32 i=0;i<6;++i)CHECK(expanded[i].scalar==adjacent[i]&&expanded[i].source==i);
    CHECK(sfnt_indic_expand_cluster(profile,{}, {},&result)==SfntStatus::ok&&!result.scalar_count&&!result.source_units);
    expanded[0]={0xc48,0xffffffffu};
    CHECK(sfnt_indic_expand_cluster(profile,{&expanded[0].scalar,1},{expanded,1},&result)==SfntStatus::invalid_input&&!result.scalar_count&&!result.source_units);
    CHECK(expanded[0].scalar==0xc48&&expanded[0].source==0xffffffffu);
    const u32 second_bindu[]{0xc05,0xc4d,0x953,0xc00},joined_bindu[]{0xc12,0xc4d,0x953,0xa0,0xc00};
    const u8 second_boundaries[]{1,0,0,1},joined_boundaries[]{1,0,0,0,1};
    CHECK(sfnt_indic_cluster_boundaries(profile,{second_bindu,4},{boundaries,4},1000000,{telugu_input_forms,128})==SfntStatus::ok&&bytes_equal(boundaries,second_boundaries,4));
    CHECK(sfnt_indic_cluster_boundaries(profile,{joined_bindu,5},{boundaries,5},1000000,{telugu_input_forms,128})==SfntStatus::ok&&bytes_equal(boundaries,joined_boundaries,5));
    NativeIndicFormPlan unprepared{};unprepared.indic.profile=profile;
    unprepared.indic.cmap=reinterpret_cast<const SfntCmap*>(usize{1});
    const NativeRunPlanStore absent{};SfntShapeRunResult shape{};NativeAsciiResult compact{};NativeParagraphShape paragraph{};
    CHECK(indic_owned_run(profile,absent,unprepared,{nullptr,1},16,SfntShapeStorage{},&shape,{},false,0)==SfntStatus::invalid_input&&!shape.glyph_count);
    CHECK(indic_owned_run(profile,absent,unprepared,{nullptr,1},16,NativeAsciiStorage{},&compact,{},false,0)==SfntStatus::invalid_input&&!compact.glyph_count);
    CHECK(indic_form_paragraph(profile,absent,unprepared,{nullptr,1},16,{}, {},&paragraph,0,false)==SfntStatus::invalid_input);
    print("PASS Telugu frozen boundaries, font forms, long input, AI decomposition, source mapping and atomic limits\n");return true;
}
