#include "maicord/text/font_metrics.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord {namespace {
#include "generated/simulation_metrics_fixture.inc"
bool equal(const SfntDesignGlyphMetrics& a,const SfntDesignGlyphMetrics& b) noexcept {
    return a.left_bearing==b.left_bearing&&a.advance_width==b.advance_width&&a.right_bearing==b.right_bearing&&
        a.top_bearing==b.top_bearing&&a.advance_height==b.advance_height&&a.bottom_bearing==b.bottom_bearing&&a.vertical_origin==b.vertical_origin;
}
SfntDesignGlyphMetrics read(const u8* bytes) noexcept {
    return {static_cast<i32>(byte_load_le32(bytes)),byte_load_le32(bytes+4),static_cast<i32>(byte_load_le32(bytes+8)),
        static_cast<i32>(byte_load_le32(bytes+12)),byte_load_le32(bytes+16),static_cast<i32>(byte_load_le32(bytes+20)),static_cast<i32>(byte_load_le32(bytes+24))};
}
bool corpus() noexcept {
    const auto* bytes=simulation_metrics_fixture;const u32 count=byte_load_le32(bytes+8);
    if(count!=1792||sizeof(simulation_metrics_fixture)!=12+static_cast<usize>(count)*64)return false;
    for(u32 pass=0;pass<2;++pass)for(u32 i=0;i<count;++i){
        const auto* row=bytes+12+(pass?count-i-1:i)*64;
        const u16 units=static_cast<u16>(byte_load_le32(row));const auto mode=static_cast<FontSimulation>(byte_load_le32(row+4));
        const auto base=read(row+8),expected=read(row+36);SfntDesignGlyphMetrics output{};
        if(sfnt_simulate_metrics(base,units,mode,&output)!=SfntStatus::ok||!equal(output,expected))return false;
        auto aliased=base;
        if(sfnt_simulate_metrics(aliased,units,mode,&aliased)!=SfntStatus::ok||!equal(aliased,expected))return false;
    }
    return true;
}
bool invalid() noexcept {
    const SfntDesignGlyphMetrics base{100,800,300,100,1000,200,800};
    SfntDesignGlyphMetrics output{};
    if(sfnt_simulate_metrics(base,1000,FontSimulation::none,nullptr)!=SfntStatus::invalid_input)return false;
    for(u32 mode=4;mode<256;++mode){output=base;
        if(sfnt_simulate_metrics(base,1000,static_cast<FontSimulation>(mode),&output)!=SfntStatus::invalid_input||!equal(output,{}))return false;
    }
    for(u32 units=0;units<65536;++units){if(units>=16&&units<=16384)continue;output=base;
        if(sfnt_simulate_metrics(output,static_cast<u16>(units),FontSimulation::bold,&output)!=SfntStatus::invalid_input||!equal(output,{}))return false;
    }
    const SfntDesignGlyphMetrics bad[]{
        {801,800,0,100,1000,200,800},{100,800,300,1001,1000,0,800},
        {0,0xffffffffu,0,100,1000,200,800},{100,800,300,0,0xffffffffu,0,800},
        {0,0,-2147483647-1,100,1000,200,800},{100,800,300,-2147483647-1,0,0,800},
        {100,800,300,100,1000,200,2147483647}};
    for(const auto& input:bad){output=base;
        if(sfnt_simulate_metrics(input,1000,FontSimulation::bold,&output)!=SfntStatus::malformed||!equal(output,{}))return false;
    }
    const SfntDesignGlyphMetrics slanted[]{
        {2147483647,0xffffffffu,0,100,1000,200,2147483647},
        {0,800,-2147483647-1,100,1000,200,2147483647},
        {-2147483647-1,800,0,100,1000,200,-2147483647-1},
        {0,0xffffffffu,2147483647,100,1000,200,-2147483647-1}};
    for(const auto& input:slanted){output=base;
        if(sfnt_simulate_metrics(input,1000,FontSimulation::oblique,&output)!=SfntStatus::malformed||!equal(output,{}))return false;
    }
    // Degenerate ink receives no simulation, even with maximum advances.
    const SfntDesignGlyphMetrics flat{2147483647,0xffffffffu,-2147483647-1,100,1000,900,2147483647};
    if(sfnt_simulate_metrics(flat,16384,FontSimulation::bold_oblique,&output)!=SfntStatus::ok||!equal(output,flat))return false;
    return true;
}
}
bool font_simulation_tests() noexcept {return corpus()&&invalid();}
}
