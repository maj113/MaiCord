#include "generated/bengali_input_fixture.inc"
bool bengali_input_units() noexcept {
    constexpr auto profile=SfntIndicProfile::bengali;
    CHECK(indic_input_corpus(profile,bengali_input_fixture_resource,bengali_input_fixture_words,bengali_input_fixture_cases,"Bengali"));
    u8 boundaries[256]{};
    const u32 input[]{0x995,0x9cb,0x9cc,0x994,0x1f600,0x9cb,0xbca};
    const SfntIndicInputScalar expected[]{{0x995,0},{0x9c7,1},{0x9be,1},{0x9c7,2},{0x9d7,2},{0x994,3},{0x1f600,4},{0x9c7,6},{0x9be,6},{0xbca,7}};
    SfntIndicInputScalar output[10]{};SfntIndicInputResult result{};
    CHECK(sfnt_indic_expand_cluster(profile,{input,7},{output,10},&result,21)==SfntStatus::ok);
    CHECK(result.scalar_count==10&&result.source_units==8&&bytes_equal(output,expected,sizeof(expected)));
    for(u64 work=0;work<21;++work){for(auto& value:output)value={0xffffffffu,0xffffffffu};
        CHECK(sfnt_indic_expand_cluster(profile,{input,7},{output,10},&result,work)==SfntStatus::resource_limit&&!result.scalar_count&&!result.source_units);
        for(auto value:output)CHECK(value.scalar==0xffffffffu&&value.source==0xffffffffu);}
    CHECK(sfnt_indic_expand_cluster(profile,{input,7},{output,9},&result)==SfntStatus::output_too_small&&result.scalar_count==10&&result.source_units==8);
    for(auto value:output)CHECK(value.scalar==0xffffffffu&&value.source==0xffffffffu);
    const u32 invalid[]{0x9cb,0xd800};
    CHECK(sfnt_indic_expand_cluster(profile,{invalid,2},{output,10},&result)==SfntStatus::invalid_input);
    CHECK(sfnt_indic_expand_cluster(static_cast<SfntIndicProfile>(255),{input,7},{output,10},&result)==SfntStatus::invalid_input);
    CHECK(sfnt_indic_expand_cluster(profile,{reinterpret_cast<const u32*>(output),1},{output,10},&result)==SfntStatus::invalid_input);
    CHECK(sfnt_indic_expand_cluster(profile,{nullptr,1},{output,10},&result)==SfntStatus::invalid_input);
    CHECK(sfnt_indic_expand_cluster(profile,{input,7},{nullptr,10},&result)==SfntStatus::invalid_input);
    CHECK(sfnt_indic_expand_cluster(profile,{input,7},{output,10},nullptr)==SfntStatus::invalid_input);
    for(auto value:output)CHECK(value.scalar==0xffffffffu&&value.source==0xffffffffu);
    CHECK(sfnt_indic_expand_cluster(profile,{},{},&result,0)==SfntStatus::ok&&!result.scalar_count&&!result.source_units);
    CHECK(sfnt_indic_expand_cluster(profile,{input,7},{},&result)==SfntStatus::output_too_small&&result.scalar_count==10);
    const u32 chain[]{0x995,0x9cd,0x995,0x9cb};
    for(u64 work=0;work<16;++work){for(auto& value:boundaries)value=255;
        CHECK(sfnt_indic_cluster_boundaries(profile,{chain,4},{boundaries,256},work)==SfntStatus::resource_limit);
        for(auto value:boundaries)CHECK(value==255);}
    CHECK(sfnt_indic_cluster_boundaries(profile,{chain,4},{boundaries,3})==SfntStatus::output_too_small);
    CHECK(sfnt_indic_cluster_boundaries(profile,{invalid,2},{boundaries,256})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(SfntIndicProfile::tamil,{chain,4},{boundaries,256})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{nullptr,4},{boundaries,256})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{chain,4},{nullptr,4})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{chain,4},{reinterpret_cast<u8*>(const_cast<u32*>(chain)),4})==SfntStatus::invalid_input);
    for(auto value:boundaries)CHECK(value==255);
    CHECK(sfnt_indic_cluster_boundaries(profile,{},{},0)==SfntStatus::ok);
    static u32 long_input[32768];static u8 long_output[32768];
    for(u32 i=0;i<32768;++i)long_input[i]=i%2?0x9cbu:0x995u;
    CHECK(sfnt_indic_cluster_boundaries(profile,{long_input,32768},{long_output,32768},32768*4)==SfntStatus::ok);
    for(u32 i=0;i<32768;++i)CHECK(long_output[i]==(i%2==0));
    print("PASS Bengali source expansion, frozen boundaries, atomic errors and long input\n");return true;
}
