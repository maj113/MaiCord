#include "generated/odia_input_fixture.inc"
bool odia_input_units() noexcept {
    const auto odia_input_fixture=indic_input_fixture(odia_input_fixture_resource);
    CHECK(odia_input_fixture.count==odia_input_fixture_words);
    constexpr auto profile=SfntIndicProfile::odia;
    u32 scalars[256]{};u8 boundaries[256]{};u32 first=0,count=0,cases=0;
    for(u32 i=0;i<odia_input_fixture.count;++i){const u32 entry=odia_input_fixture[i];CHECK(count<256);scalars[count++]=entry&0x1fffffu;
        if(entry&0x40000000u){CHECK(sfnt_indic_cluster_boundaries(profile,{scalars,count},{boundaries,count})==SfntStatus::ok);
            for(u32 j=0;j<count;++j)if(boundaries[j]!=(odia_input_fixture[first+j]>>31)){print("Odia fixture case ");number(cases);print("\n");CHECK(false);}
            ++cases;count=0;first=i+1;}}
    CHECK(!count&&cases==odia_input_fixture_cases);
    const u32 clustered[]{0xb30,0xb4d,0xb15,0xb48},invalid_cluster[]{0xb15,0xd800};
    for(auto& value:boundaries)value=255;
    CHECK(sfnt_indic_cluster_boundaries(profile,{clustered,4},{boundaries,256},15)==SfntStatus::resource_limit);
    CHECK(sfnt_indic_cluster_boundaries(profile,{clustered,4},{boundaries,3})==SfntStatus::output_too_small);
    CHECK(sfnt_indic_cluster_boundaries(profile,{invalid_cluster,2},{boundaries,256})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{nullptr,1},{boundaries,256})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{clustered,4},{nullptr,4})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{clustered,4},{reinterpret_cast<u8*>(const_cast<u32*>(clustered)),4})==SfntStatus::invalid_input);
    for(auto value:boundaries)CHECK(value==255);
    CHECK(sfnt_indic_cluster_boundaries(profile,{},{},0)==SfntStatus::ok);
    static u32 long_input[32768];static u8 long_output[32768];
    for(u32 i=0;i<32768;++i)long_input[i]=i%2?0xb48u:0xb15u;
    CHECK(sfnt_indic_cluster_boundaries(profile,{long_input,32768},{long_output,32768},32768*4)==SfntStatus::ok);
    for(u32 i=0;i<32768;++i)CHECK(long_output[i]==(i%2==0));
    // Unicode 17.0 canonical dependent-vowel decompositions. Shaping keeps
    // both pieces attached to the original source; independent vowels stay whole.
    const u32 pairs[][3]{{0xb48,0xb47,0xb56},{0xb4b,0xb47,0xb3e},{0xb4c,0xb47,0xb57}};
    SfntIndicInputScalar expanded[131]{};SfntIndicInputResult result{};
    for(const auto& pair:pairs){const u32 input[]{0x1f600,0xb15,pair[0],0x200d,0xb10};
        const SfntIndicInputScalar expected[]{{0x1f600,0},{0xb15,2},{pair[1],3},{pair[2],3},{0x200d,4},{0xb10,5}};
        for(u64 work=0;work<=15;++work){for(auto& value:expanded)value={0xffffffffu,0xffffffffu};
            const auto status=sfnt_indic_expand_cluster(profile,{input,5},{expanded,131},&result,work);
            CHECK(status==(work==15?SfntStatus::ok:SfntStatus::resource_limit));
            if(work==15)CHECK(result.scalar_count==6&&result.source_units==6&&bytes_equal(expanded,expected,sizeof(expected)));
            else {CHECK(!result.scalar_count&&!result.source_units);for(const auto& value:expanded)CHECK(value.scalar==0xffffffffu&&value.source==0xffffffffu);}}
        for(auto& value:expanded)value={0xffffffffu,0xffffffffu};
        CHECK(sfnt_indic_expand_cluster(profile,{input,5},{expanded,5},&result)==SfntStatus::output_too_small&&result.scalar_count==6&&result.source_units==6);
        for(const auto& value:expanded)CHECK(value.scalar==0xffffffffu&&value.source==0xffffffffu);
        const u32 invalid[]{pair[0],0xd800};
        CHECK(sfnt_indic_expand_cluster(profile,{invalid,2},{expanded,131},&result)==SfntStatus::invalid_input&&!result.scalar_count&&!result.source_units);
        for(const auto& value:expanded)CHECK(value.scalar==0xffffffffu&&value.source==0xffffffffu);
    }
    u32 block[128]{};for(u32 i=0;i<128;++i)block[i]=0xb00+i;
    CHECK(sfnt_indic_expand_cluster(profile,{block,128},{expanded,131},&result)==SfntStatus::ok&&result.scalar_count==131&&result.source_units==128);
    u32 at=0;for(u32 i=0;i<128;++i){const u32 scalar=block[i];const u32* parts=nullptr;
        for(const auto& pair:pairs)if(pair[0]==scalar)parts=pair;
        if(parts){CHECK(expanded[at].scalar==parts[1]&&expanded[at].source==i);++at;
            CHECK(expanded[at].scalar==parts[2]&&expanded[at].source==i);++at;}
        else {CHECK(expanded[at].scalar==scalar&&expanded[at].source==i);++at;}}
    CHECK(at==131);
    // The observed 31-slot preparation policy spends added pieces from left
    // to right. The generic expander keeps unsplit originals and their sources.
    const u32 limited[]{0xb15,0xb4b,0xb4c};
    const SfntIndicInputScalar one_extra[]{{0xb15,0},{0xb47,1},{0xb3e,1},{0xb4c,2}};
    CHECK(sfnt_indic_expand_cluster(profile,{limited,3},{expanded,131},&result,9,1)==SfntStatus::ok&&result.scalar_count==4&&result.source_units==3);
    CHECK(bytes_equal(expanded,one_extra,sizeof(one_extra)));
    CHECK(sfnt_indic_expand_cluster(profile,{limited,3},{expanded,131},&result,9,0)==SfntStatus::ok&&result.scalar_count==3&&result.source_units==3);
    for(u32 i=0;i<3;++i)CHECK(expanded[i].scalar==limited[i]&&expanded[i].source==i);
    for(auto& value:expanded)value={0xffffffffu,0xffffffffu};
    CHECK(sfnt_indic_expand_cluster(profile,{limited,3},{expanded,3},&result,9,1)==SfntStatus::output_too_small&&result.scalar_count==4&&result.source_units==3);
    for(const auto& value:expanded)CHECK(value.scalar==0xffffffffu&&value.source==0xffffffffu);
    const u32 adjacent[]{0x9cb,0xbca,0xbcb,0xbcc};
    CHECK(sfnt_indic_expand_cluster(profile,{adjacent,4},{expanded,131},&result)==SfntStatus::ok&&result.scalar_count==4&&result.source_units==4);
    for(u32 i=0;i<4;++i)CHECK(expanded[i].scalar==adjacent[i]&&expanded[i].source==i);
    CHECK(sfnt_indic_expand_cluster(profile,{}, {},&result)==SfntStatus::ok&&!result.scalar_count&&!result.source_units);
    expanded[0]={0xb48,0xffffffffu};
    CHECK(sfnt_indic_expand_cluster(profile,{&expanded[0].scalar,1},{expanded,1},&result)==SfntStatus::invalid_input&&!result.scalar_count&&!result.source_units);
    CHECK(expanded[0].scalar==0xb48&&expanded[0].source==0xffffffffu);
    print("PASS Odia frozen boundaries, long input, dependent-vowel expansion, UTF-16 sources and atomic failures\n");return true;
}
