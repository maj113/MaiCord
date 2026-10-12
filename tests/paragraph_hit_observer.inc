// Optional bounded observation stream for offline point-boundary investigation.
// Mode 1 observes only; mode 2 also verifies native points at each coordinate.
// Normal acceptance suites clear this environment override.
struct HitObservationWriter {
    char bytes[32768];u32 used=0;bool ok=true;
    void flush() noexcept {DWORD written=0;if(used)ok=ok&&WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),bytes,used,&written,nullptr)&&written==used;used=0;}
    void text(const char* value) noexcept {while(*value){if(used==sizeof(bytes))flush();bytes[used++]=*value++;}}
    void number(u32 value) noexcept {char digits[12];u32 count=0;do{digits[count++]=static_cast<char>('0'+value%10);value/=10;}while(value);while(count){const char one[]{digits[--count],0};text(one);}}
    void real(float value) noexcept {u32 bits=0;copy_bytes(&bits,&value,4);number(bits);}
};
bool observe_hit_points(const NativeParagraphHits& h,IDWriteTextLayout* layout,Utf8View text,float em,float tracking,ConstSlice<TextStyleSpan> styles,bool* observed) noexcept {
    wchar_t enabled[2]{};*observed=GetEnvironmentVariableW(L"MAICORD_HIT_OBSERVE",enabled,2)==1&&(enabled[0]=='1'||enabled[0]=='2');if(!*observed)return true;
    HitObservationWriter w;w.text(enabled[0]=='2'?"{\"kind\":\"hit_boundary_verified\",\"em\":" : "{\"kind\":\"hit_observation_only\",\"em\":");w.real(em);w.text(",\"tracking\":");w.real(tracking);
    w.text(",\"text\":[");bool comma=false;
    for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);CHECK(cp.valid);if(comma)w.text(",");comma=true;w.number(cp.codepoint);at+=cp.width;}
    w.text("],\"styles\":[");for(u32 i=0;i<styles.count;++i){if(i)w.text(",");w.text("[");w.number(styles[i].byte_offset);w.text(",");w.number(styles[i].byte_count);w.text(",");w.number(styles[i].flags);w.text("]");}
    w.text("],\"clusters\":[");for(u32 i=0;i<h.clusters.count;++i){const auto& c=h.clusters[i];ConstSlice<float> cluster_advances{};CHECK(native_paragraph_hit_advances(h,i,&cluster_advances));if(i)w.text(",");w.text("[");w.number(c.source_first);w.text(",");w.number(c.source_end);w.text(",");w.number(c.advance_flags&native_hit_advance_mask);w.text(",");w.number((c.advance_flags&~native_hit_advance_mask)|cluster_advances.count);w.text(",");w.real(c.left);w.text(",");w.real(c.width);w.text(",");w.number(c.style);w.text("]");}
    w.text("],\"advances\":[");for(u32 i=0;i<h.advances.count;++i){if(i)w.text(",");w.real(h.advances[i]);}
    w.text("],\"runs\":[");for(u32 i=0;i<h.visual_runs.count;++i){const auto& r=h.visual_runs[i];if(i)w.text(",");w.text("[");w.number(r.first_cluster);w.text(",");w.number(r.cluster_count);w.text(",");w.real(r.left);w.text(",");w.real(r.width);w.text(",");w.number(r.bidi_level);w.text("]");}
    w.text("],\"lines\":[");for(u32 i=0;i<h.lines.count;++i){const auto& line=h.lines[i];if(i)w.text(",");w.text("[");w.number(line.first_cluster);w.text(",");w.number(line.cluster_count);w.text(",");w.real(line.top);w.text(",");w.real(line.caret_end);w.text("]");}
    w.text("],\"points\":[");comma=false;begin_hit_point_probes();
    for(u32 position=0;position<h.source_units;++position){DWRITE_HIT_TEST_METRICS m{};float px=0,py=0;CHECK(SUCCEEDED(layout->HitTestTextPosition(position,FALSE,&px,&py,&m)));
        const float centers[]{m.left,m.left+m.width*.5f,m.left+m.width};
        for(float center:centers){u32 bits=0;copy_bytes(&bits,&center,4);
            for(i32 delta=-4;delta<=4;++delta){const u32 encoded=bits+static_cast<u32>(delta);float x=0;copy_bytes(&x,&encoded,4);
                if((encoded&0x7f800000u)==0x7f800000u||repeated_hit_point(x,py))continue;
                BOOL trailing=FALSE,inside=FALSE;DWRITE_HIT_TEST_METRICS hit{};CHECK(SUCCEEDED(layout->HitTestPoint(x,py,&trailing,&inside,&hit)));
                if(enabled[0]=='2'){NativeParagraphHitPoint actual{};CHECK(native_paragraph_hit_point(h,x,py,&actual)==SfntStatus::ok);
                    if(actual.source_position!=hit.textPosition+(trailing?hit.length:0)||actual.inside!=(inside!=FALSE)){print("Adjacent-boundary mismatch x=");real(x);print("\n");CHECK(false);}}
                if(comma)w.text(",");comma=true;w.text("[");w.number(encoded);w.text(",");w.real(py);w.text(",");w.number(hit.textPosition);w.text(",");w.number(hit.length);w.text(",");w.number(trailing);w.text(",");w.number(inside);w.text(",");w.real(hit.left);w.text(",");w.real(hit.width);w.text("]");
            }}
    }
    w.text("]}\n");w.flush();CHECK(w.ok);return true;
}
