#include "generated/malayalam_input_fixture.inc"
bool malayalam_input_units() noexcept {
    const auto malayalam_input_fixture=indic_input_fixture(malayalam_input_fixture_resource);
    CHECK(malayalam_input_fixture.count==malayalam_input_fixture_words);
    constexpr auto profile=SfntIndicProfile::malayalam;
    const ConstSlice<u8> forms{malayalam_input_forms,128};u32 scalars[256]{};u8 boundaries[256]{};u32 first=0,count=0,cases=0;
    for(u32 i=0;i<malayalam_input_fixture.count;++i){const u32 entry=malayalam_input_fixture[i];CHECK(count<256);scalars[count++]=entry&0x1fffffu;
        if(entry&0x40000000u){CHECK(sfnt_indic_cluster_boundaries(profile,{scalars,count},{boundaries,256},1000000,forms)==SfntStatus::ok);
            for(u32 j=0;j<count;++j)if(boundaries[j]!=(malayalam_input_fixture[first+j]>>31)){print("Malayalam fixture case ");number(cases);print("\n");CHECK(false);}
            ++cases;count=0;first=i+1;}}
    CHECK(!count&&cases==malayalam_input_fixture_cases);
    const u32 post_vowel[]{0xd0e,0xd4d,0xd2f,0xd46};const u8 post_boundaries[]{1,0,0,1};
    CHECK(sfnt_indic_cluster_boundaries(profile,{post_vowel,4},{boundaries,256},16,forms)==SfntStatus::ok&&bytes_equal(boundaries,post_boundaries,4));
    for(auto& value:boundaries)value=255;
    for(u64 work=0;work<16;++work)CHECK(sfnt_indic_cluster_boundaries(profile,{post_vowel,4},{boundaries,256},work,forms)==SfntStatus::resource_limit);
    const u32 invalid_input[]{0xd15,0xd800};
    CHECK(sfnt_indic_cluster_boundaries(profile,{invalid_input,2},{boundaries,256},1000000,forms)==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{post_vowel,4},{boundaries,3},1000000,forms)==SfntStatus::output_too_small);
    CHECK(sfnt_indic_cluster_boundaries(profile,{nullptr,1},{boundaries,256},1000000,forms)==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{post_vowel,4},{nullptr,4},1000000,forms)==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{post_vowel,4},{boundaries,256})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{post_vowel,4},{boundaries,256},1000000,{forms.data,127})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{post_vowel,4},{boundaries,256},1000000,{nullptr,128})==SfntStatus::invalid_input);
    for(auto value:boundaries)CHECK(value==255);
    copy_bytes(scalars,post_vowel,sizeof(post_vowel));
    CHECK(sfnt_indic_cluster_boundaries(profile,{scalars,4},{reinterpret_cast<u8*>(scalars),4},1000000,forms)==SfntStatus::invalid_input&&bytes_equal(scalars,post_vowel,sizeof(post_vowel)));
    u8 alias_forms[128]{};
    CHECK(sfnt_indic_cluster_boundaries(profile,{post_vowel,4},{alias_forms,4},1000000,{alias_forms,128})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{},{},0,forms)==SfntStatus::ok);
    static u32 long_input[32768];static u8 long_output[32768];
    for(u32 i=0;i<32768;++i)long_input[i]=i%2?0xd3fu:0xd15u;
    CHECK(sfnt_indic_cluster_boundaries(profile,{long_input,32768},{long_output,32768},32768*4,forms)==SfntStatus::ok);
    for(u32 i=0;i<32768;++i)CHECK(long_output[i]==(i%2==0));
    // Defaults are selected once after raw font queries. One actual post form,
    // even on another consonant, keeps every other raw class unchanged.
    u8 fallback_forms[128]{};fallback_forms[0x30]=sfnt_deva_pre;indic_form_defaults(profile,fallback_forms);
    for(u32 i=0;i<128;++i){const u8 expected=i==0x30?sfnt_deva_pre|sfnt_deva_post:i==0x2f||i==0x35?sfnt_deva_post:i==0x32?sfnt_deva_below:0;CHECK(fallback_forms[i]==(expected?expected|indic_default_position:0));}
    const u32 default_consonants[]{0xd2f,0xd30,0xd32,0xd35};
    for(u32 consonant:default_consonants){const u32 fallback_text[]{0xd0e,0xd4d,consonant,0xd46};
        CHECK(sfnt_indic_cluster_boundaries(profile,{fallback_text,4},{boundaries,256},16,{fallback_forms,128})==SfntStatus::ok&&bytes_equal(boundaries,post_boundaries,4));}
    for(auto& value:fallback_forms)value=0;fallback_forms[0x15]=sfnt_deva_post;
    indic_form_defaults(profile,fallback_forms);for(u32 i=0;i<128;++i)CHECK(fallback_forms[i]==(i==0x15?sfnt_deva_post:0));
    indic_form_defaults(SfntIndicProfile::kannada,fallback_forms);for(u32 i=0;i<128;++i)CHECK(fallback_forms[i]==(i==0x15?sfnt_deva_post:0));
    const u32 split[][3]{{0xd4a,0xd46,0xd3e},{0xd4b,0xd47,0xd3e},{0xd4c,0xd46,0xd57}};
    SfntIndicInputScalar output[131]{};SfntIndicInputResult result{};
    const auto clear=[&]() noexcept {for(auto& s:output)s={0xffffffffu,0xffffffffu};};
    const auto untouched=[&]() noexcept {for(const auto& s:output)if(s.scalar!=0xffffffffu||s.source!=0xffffffffu)return false;return true;};
    for(const auto& d:split){const u32 input[]{0x1f600,0xd15,d[0],0x200d,0xd7a};
        const SfntIndicInputScalar expected[]{{0x1f600,0},{0xd15,2},{d[1],3},{d[2],3},{0x200d,4},{0xd7a,5}};
        for(u64 work=0;work<=15;++work){clear();
            CHECK(sfnt_indic_expand_cluster(profile,{input,5},{output,131},&result,work)==(work==15?SfntStatus::ok:SfntStatus::resource_limit));
            if(work==15)CHECK(result.scalar_count==6&&result.source_units==6&&bytes_equal(output,expected,sizeof(expected)));
            else CHECK(!result.scalar_count&&!result.source_units&&untouched());}
        for(u32 capacity=0;capacity<6;++capacity){clear();
            CHECK(sfnt_indic_expand_cluster(profile,{input,5},{output,capacity},&result)==SfntStatus::output_too_small);
            CHECK(result.scalar_count==6&&result.source_units==6&&untouched());}
        const u32 invalid[]{d[0],0xd800};clear();
        CHECK(sfnt_indic_expand_cluster(profile,{invalid,2},{output,131},&result)==SfntStatus::invalid_input&&!result.scalar_count&&!result.source_units&&untouched());
    }
    u32 block[128]{};for(u32 i=0;i<128;++i)block[i]=0xd00+i;
    CHECK(sfnt_indic_expand_cluster(profile,{block,128},{output,131},&result)==SfntStatus::ok&&result.scalar_count==131&&result.source_units==128);
    u32 at=0;for(u32 i=0;i<128;++i){const u32* parts=nullptr;for(const auto& d:split)if(block[i]==d[0])parts=d;
        if(parts){for(u32 j=1;j<3;++j){CHECK(output[at].scalar==parts[j]&&output[at].source==i);++at;}}
        else{CHECK(output[at].scalar==block[i]&&output[at].source==i);++at;}}
    CHECK(at==131); // Chillus, dot reph, archaic viramas and AU length remain atomic.
    const u32 limited[]{0xd4a,0xd4b,0xd4c};
    for(u32 limit=0;limit<=3;++limit){
        CHECK(sfnt_indic_expand_cluster(profile,{limited,3},{output,131},&result,9,limit)==SfntStatus::ok&&result.scalar_count==3+limit&&result.source_units==3);
        u32 slot=0;for(u32 i=0;i<3;++i){if(i<limit){CHECK(output[slot].scalar==split[i][1]&&output[slot++].source==i);CHECK(output[slot].scalar==split[i][2]&&output[slot++].source==i);}
            else CHECK(output[slot].scalar==limited[i]&&output[slot++].source==i);}CHECK(slot==3+limit);}
    const u32 neighbors[]{0x9cb,0xb48,0xbca,0xc48,0xccb,0xdda};
    CHECK(sfnt_indic_expand_cluster(profile,{neighbors,6},{output,131},&result)==SfntStatus::ok&&result.scalar_count==6);
    for(u32 i=0;i<6;++i)CHECK(output[i].scalar==neighbors[i]&&output[i].source==i);
    const SfntIndicProfile others[]{SfntIndicProfile::devanagari,SfntIndicProfile::tamil,SfntIndicProfile::bengali,SfntIndicProfile::gujarati,SfntIndicProfile::gurmukhi,SfntIndicProfile::odia,SfntIndicProfile::telugu,SfntIndicProfile::kannada};
    for(auto other:others){CHECK(sfnt_indic_expand_cluster(other,{limited,3},{output,131},&result)==SfntStatus::ok&&result.scalar_count==3);
        for(u32 i=0;i<3;++i)CHECK(output[i].scalar==limited[i]&&output[i].source==i);}
    clear();CHECK(sfnt_indic_expand_cluster(profile,{nullptr,1},{output,131},&result)==SfntStatus::invalid_input&&untouched());
    CHECK(sfnt_indic_expand_cluster(profile,{limited,3},{nullptr,6},&result)==SfntStatus::invalid_input);
    CHECK(sfnt_indic_expand_cluster(profile,{},{},&result,0)==SfntStatus::ok&&!result.scalar_count&&!result.source_units);
    output[0]={0xd4a,0xffffffffu};
    CHECK(sfnt_indic_expand_cluster(profile,{&output[0].scalar,1},{output,131},&result)==SfntStatus::invalid_input&&!result.scalar_count&&!result.source_units);
    CHECK(output[0].scalar==0xd4a&&output[0].source==0xffffffffu);
    u8 boundary=255;const u32 consonant=0xd15;
    CHECK(sfnt_indic_cluster_boundaries(profile,{&consonant,1},{&boundary,1},4,forms)==SfntStatus::ok&&boundary==1);
    print("PASS Malayalam frozen scanner vectors, source-preserving vowels, complete block isolation, long input and atomic failures\n");return true;
}
