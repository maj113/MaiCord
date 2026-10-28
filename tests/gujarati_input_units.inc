#include "generated/gujarati_input_fixture.inc"
bool gujarati_input_units() noexcept {
    constexpr auto profile=SfntIndicProfile::gujarati;
    CHECK(indic_input_corpus(profile,gujarati_input_fixture_resource,gujarati_input_fixture_words,gujarati_input_fixture_cases,"Gujarati"));
    u8 boundaries[256]{};
    // Gujarati signs stay nominal, including the top/right candra-O sign.
    // Adjacent Bengali/Tamil signs must not borrow those profiles' expansion.
    const u32 input[]{0xa95,0xac9,0xacb,0xacc,0x1f600,0x9cb,0xbca};
    const SfntIndicInputScalar expected[]{{0xa95,0},{0xac9,1},{0xacb,2},{0xacc,3},{0x1f600,4},{0x9cb,6},{0xbca,7}};
    SfntIndicInputScalar output[7]{};SfntIndicInputResult result{};
    CHECK(sfnt_indic_expand_cluster(profile,{input,7},{output,7},&result,21)==SfntStatus::ok);
    CHECK(result.scalar_count==7&&result.source_units==8&&bytes_equal(output,expected,sizeof(expected)));
    for(auto& value:output)value={0xffffffffu,0xffffffffu};
    CHECK(sfnt_indic_expand_cluster(profile,{input,7},{output,6},&result)==SfntStatus::output_too_small&&result.scalar_count==7&&result.source_units==8);
    const u32 invalid[]{0xac9,0xd800};
    CHECK(sfnt_indic_expand_cluster(profile,{invalid,2},{output,7},&result)==SfntStatus::invalid_input&&!result.scalar_count&&!result.source_units);
    CHECK(sfnt_indic_expand_cluster(profile,{input,7},{output,7},&result,20)==SfntStatus::resource_limit);
    CHECK(sfnt_indic_expand_cluster(profile,{reinterpret_cast<const u32*>(output),1},{output,7},&result)==SfntStatus::invalid_input);
    for(auto value:output)CHECK(value.scalar==0xffffffffu&&value.source==0xffffffffu);
    for(auto& value:boundaries)value=255;
    CHECK(sfnt_indic_cluster_boundaries(profile,{input,7},{boundaries,256},27)==SfntStatus::resource_limit);
    CHECK(sfnt_indic_cluster_boundaries(profile,{input,7},{boundaries,6})==SfntStatus::output_too_small);
    CHECK(sfnt_indic_cluster_boundaries(profile,{invalid,2},{boundaries,256})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{nullptr,1},{boundaries,256})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{input,7},{nullptr,7})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{input,7},{reinterpret_cast<u8*>(const_cast<u32*>(input)),7})==SfntStatus::invalid_input);
    for(auto value:boundaries)CHECK(value==255);
    CHECK(sfnt_indic_cluster_boundaries(profile,{},{},0)==SfntStatus::ok);
    static u32 long_input[32768];static u8 long_output[32768];
    for(u32 i=0;i<32768;++i)long_input[i]=i%2?0xacbu:0xa95u;
    CHECK(sfnt_indic_cluster_boundaries(profile,{long_input,32768},{long_output,32768},32768*4)==SfntStatus::ok);
    for(u32 i=0;i<32768;++i)CHECK(long_output[i]==(i%2==0));
    print("PASS Gujarati frozen boundaries, nominal signs, source identity, atomic failures and long input\n");return true;
}
