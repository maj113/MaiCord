#include "generated/kannada_input_fixture.inc"
bool kannada_input_units() noexcept {
    constexpr auto profile=SfntIndicProfile::kannada;
    const ConstSlice<u8> forms{kannada_input_forms,128};
    CHECK(indic_input_corpus(profile,kannada_input_fixture_resource,kannada_input_fixture_words,kannada_input_fixture_cases,"Kannada",forms));
    u32 scalars[256]{};u8 boundaries[256]{};
    // Independent Windows observations with feature-disabled font fixtures.
    const u32 vowel_subscript[]{0xc92,0xccd,0xc95,0xccc},reph_vowel[]{0xcb0,0xccd,0xc85};
    const u8 vowel_normal[]{1,0,0,1},vowel_no_below[]{1,0,0,0},reph_normal[]{1,0,0},reph_absent[]{1,0,1};
    u8 altered_forms[128];
    CHECK(sfnt_indic_cluster_boundaries(profile,{vowel_subscript,4},{boundaries,256},16,forms)==SfntStatus::ok&&bytes_equal(boundaries,vowel_normal,4));
    for(u32 i=0;i<128;++i)altered_forms[i]=static_cast<u8>(kannada_input_forms[i]&~sfnt_deva_below);
    CHECK(sfnt_indic_cluster_boundaries(profile,{vowel_subscript,4},{boundaries,256},16,{altered_forms,128})==SfntStatus::ok&&bytes_equal(boundaries,vowel_no_below,4));
    CHECK(sfnt_indic_cluster_boundaries(profile,{reph_vowel,3},{boundaries,256},12,forms)==SfntStatus::ok&&bytes_equal(boundaries,reph_normal,3));
    for(u32 i=0;i<128;++i)altered_forms[i]=static_cast<u8>(kannada_input_forms[i]&~sfnt_deva_reph);
    CHECK(sfnt_indic_cluster_boundaries(profile,{reph_vowel,3},{boundaries,256},12,{altered_forms,128})==SfntStatus::ok&&bytes_equal(boundaries,reph_absent,3));
    for(auto& value:boundaries)value=255;
    for(u64 work=0;work<16;++work)CHECK(sfnt_indic_cluster_boundaries(profile,{vowel_subscript,4},{boundaries,256},work,forms)==SfntStatus::resource_limit);
    const u32 invalid_input[]{0xc95,0xd800};
    CHECK(sfnt_indic_cluster_boundaries(profile,{invalid_input,2},{boundaries,256},1000000,forms)==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{vowel_subscript,4},{boundaries,3},1000000,forms)==SfntStatus::output_too_small);
    CHECK(sfnt_indic_cluster_boundaries(profile,{nullptr,1},{boundaries,256},1000000,forms)==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{vowel_subscript,4},{nullptr,4},1000000,forms)==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{vowel_subscript,4},{boundaries,256})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{vowel_subscript,4},{boundaries,256},1000000,{altered_forms,127})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{vowel_subscript,4},{boundaries,256},1000000,{nullptr,128})==SfntStatus::invalid_input);
    for(auto value:boundaries)CHECK(value==255);
    copy_bytes(scalars,vowel_subscript,sizeof(vowel_subscript));
    CHECK(sfnt_indic_cluster_boundaries(profile,{scalars,4},{reinterpret_cast<u8*>(scalars),4},1000000,forms)==SfntStatus::invalid_input&&bytes_equal(scalars,vowel_subscript,sizeof(vowel_subscript)));
    CHECK(sfnt_indic_cluster_boundaries(profile,{vowel_subscript,4},{altered_forms,4},1000000,{altered_forms,128})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(profile,{},{},0,forms)==SfntStatus::ok);
    static u32 long_input[32768];static u8 long_output[32768];
    for(u32 i=0;i<32768;++i)long_input[i]=i%2?0xcbfu:0xc95u;
    CHECK(sfnt_indic_cluster_boundaries(profile,{long_input,32768},{long_output,32768},32768*4,forms)==SfntStatus::ok);
    for(u32 i=0;i<32768;++i)CHECK(long_output[i]==(i%2==0));
    // Unicode 17 canonical decompositions, recursively expanded for OO.
    const u32 decompositions[][4]{{0xcc0,0xcbf,0xcd5,0},{0xcc7,0xcc6,0xcd5,0},
        {0xcc8,0xcc6,0xcd6,0},{0xcca,0xcc6,0xcc2,0},{0xccb,0xcc6,0xcc2,0xcd5}};
    SfntIndicInputScalar output[134]{};SfntIndicInputResult result{};
    const auto clear=[&]() noexcept {for(auto& value:output)value={0xffffffffu,0xffffffffu};};
    const auto untouched=[&]() noexcept {for(const auto& value:output)if(value.scalar!=0xffffffffu||value.source!=0xffffffffu)return false;return true;};
    for(const auto& d:decompositions){const u32 input[]{0x1f600,0xc95,d[0],0x200d,0xc90};
        const u32 pieces=d[3]?3u:2u,expected_count=4+pieces;
        for(u64 work=0;work<=15;++work){clear();
            const auto status=sfnt_indic_expand_cluster(profile,{input,5},{output,134},&result,work);
            CHECK(status==(work==15?SfntStatus::ok:SfntStatus::resource_limit));
            if(work<15){CHECK(!result.scalar_count&&!result.source_units&&untouched());continue;}
            CHECK(result.scalar_count==expected_count&&result.source_units==6);
            CHECK(output[0].scalar==0x1f600&&!output[0].source&&output[1].scalar==0xc95&&output[1].source==2);
            for(u32 j=0;j<pieces;++j)CHECK(output[2+j].scalar==d[1+j]&&output[2+j].source==3);
            CHECK(output[2+pieces].scalar==0x200d&&output[2+pieces].source==4&&output[3+pieces].scalar==0xc90&&output[3+pieces].source==5);
        }
        for(u32 capacity=0;capacity<expected_count;++capacity){clear();
            CHECK(sfnt_indic_expand_cluster(profile,{input,5},{output,capacity},&result)==SfntStatus::output_too_small);
            CHECK(result.scalar_count==expected_count&&result.source_units==6&&untouched());}
        const u32 invalid[]{d[0],0xd800};clear();
        CHECK(sfnt_indic_expand_cluster(profile,{invalid,2},{output,134},&result)==SfntStatus::invalid_input&&!result.scalar_count&&!result.source_units&&untouched());
    }
    u32 block[128]{};for(u32 i=0;i<128;++i)block[i]=0xc80+i;
    CHECK(sfnt_indic_expand_cluster(profile,{block,128},{output,134},&result)==SfntStatus::ok&&result.scalar_count==134&&result.source_units==128);
    u32 at=0;for(u32 i=0;i<128;++i){const u32* parts=nullptr;for(const auto& d:decompositions)if(block[i]==d[0])parts=d;
        if(parts){for(u32 j=1;j<4&&parts[j];++j){CHECK(output[at].scalar==parts[j]&&output[at].source==i);++at;}}
        else{CHECK(output[at].scalar==block[i]&&output[at].source==i);++at;}}
    CHECK(at==134);
    // A short expansion budget keeps the whole vowel and ends further splitting.
    // This is an adapter contract, not yet a Windows cluster-capacity claim.
    const u32 limited[]{0xcc7,0xccb,0xcc0};
    const SfntIndicInputScalar expected[][7]{
        {{0xcc7,0},{0xccb,1},{0xcc0,2}},
        {{0xcc6,0},{0xcd5,0},{0xccb,1},{0xcc0,2}},
        {{0xcc6,0},{0xcd5,0},{0xccb,1},{0xcc0,2}},
        {{0xcc6,0},{0xcd5,0},{0xcc6,1},{0xcc2,1},{0xcd5,1},{0xcc0,2}},
        {{0xcc6,0},{0xcd5,0},{0xcc6,1},{0xcc2,1},{0xcd5,1},{0xcbf,2},{0xcd5,2}}};
    const u32 lengths[]{3,4,4,6,7};
    for(u32 limit=0;limit<=4;++limit){
        CHECK(sfnt_indic_expand_cluster(profile,{limited,3},{output,134},&result,9,limit)==SfntStatus::ok);
        CHECK(result.scalar_count==lengths[limit]&&result.source_units==3&&bytes_equal(output,expected[limit],lengths[limit]*sizeof(output[0])));}
    const u32 neighbors[]{0x9cb,0xb48,0xbca,0xc48,0xd4b};
    CHECK(sfnt_indic_expand_cluster(profile,{neighbors,5},{output,134},&result)==SfntStatus::ok&&result.scalar_count==5);
    for(u32 i=0;i<5;++i)CHECK(output[i].scalar==neighbors[i]&&output[i].source==i);
    const SfntIndicProfile neighbors_profiles[]{SfntIndicProfile::tamil,SfntIndicProfile::bengali,SfntIndicProfile::odia,SfntIndicProfile::telugu};
    for(auto other:neighbors_profiles){
        CHECK(sfnt_indic_expand_cluster(other,{limited,3},{output,134},&result)==SfntStatus::ok&&result.scalar_count==3);
        for(u32 i=0;i<3;++i)CHECK(output[i].scalar==limited[i]&&output[i].source==i);}
    CHECK(sfnt_indic_expand_cluster(profile,{},{},&result,0)==SfntStatus::ok&&!result.scalar_count&&!result.source_units);
    clear();CHECK(sfnt_indic_expand_cluster(profile,{nullptr,1},{output,134},&result)==SfntStatus::invalid_input&&untouched());
    CHECK(sfnt_indic_expand_cluster(profile,{limited,3},{nullptr,7},&result)==SfntStatus::invalid_input);
    output[0]={0xccb,0xffffffffu};
    CHECK(sfnt_indic_expand_cluster(profile,{&output[0].scalar,1},{output,134},&result)==SfntStatus::invalid_input&&!result.scalar_count&&!result.source_units);
    CHECK(output[0].scalar==0xccb&&output[0].source==0xffffffffu);

    print("PASS Kannada frozen scanner vectors, dynamic font forms, long input, vowel expansion, source offsets and atomic failures\n");return true;
}
