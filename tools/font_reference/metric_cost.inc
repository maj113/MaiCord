// QA only: identical request stream for native callbacks and DirectWrite batches.
template<class Query,class Read,class Text,class Number>
bool metric_cost(u32 glyph_count,Query query,Read read,Text text,Number number) noexcept {
    if(!glyph_count)return false;
    u16 glyphs[256];for(u32 i=0;i<256;++i)glyphs[i]=static_cast<u16>((i+1)%glyph_count);
    LARGE_INTEGER frequency{};if(!QueryPerformanceFrequency(&frequency))return false;
    const auto field=[&](const char* name,u64 value) noexcept {text(name);number(value);};
    const u32 sizes[]{256,1,16,256};
    for(u32 phase=0;phase<4;++phase){const u32 count=sizes[phase],iterations=phase?4096:1;
        LARGE_INTEGER begin{},end{};ULONG64 first_cycles=0,last_cycles=0;
        if(!QueryThreadCycleTime(GetCurrentThread(),&first_cycles)||!QueryPerformanceCounter(&begin))return false;
        for(u32 i=0;i<iterations;++i)if(!query(glyphs,count))return false;
        if(!QueryPerformanceCounter(&end)||!QueryThreadCycleTime(GetCurrentThread(),&last_cycles))return false;
        field("{\"kind\":\"metric_cost\",\"phase\":",phase);field(",\"batch\":",count);field(",\"iterations\":",iterations);
        field(",\"frequency\":",frequency.QuadPart);field(",\"ticks\":",end.QuadPart-begin.QuadPart);field(",\"cycles\":",last_cycles-first_cycles);
        text(",\"advances\":[");for(u32 i=0;i<count;++i){if(i)text(",");const auto value=read(i);if(value<0){text("-");number(static_cast<u64>(-value));}else number(static_cast<u64>(value));}
        text("]}\n");
    }
    return true;
}
