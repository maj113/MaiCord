#include "native_source.hpp"
#include "dwrite_paint_builder.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace maicord;
namespace {
void check(HRESULT result,const char* operation) {
    if(FAILED(result)){fprintf(stderr,"%s failed: %08lx\n",operation,result);exit(1);}
}
struct Input {u16 glyph;float size;};
struct Measurement {double seconds,p50_us,p95_us,p99_us;u64 cycles,bytes;};
static NativePaintContext native;
bool build(bool use_native,ColrPaintProgram* program,IDWritePaintReader* reader,IDWriteFontFace* face,Input input) {
    if(use_native)return native.build(program,input.glyph,input.size);
    DWRITE_PAINT_ELEMENT root{};D2D_RECT_F clip{};
    return SUCCEEDED(reader->SetCurrentGlyph(input.glyph,&root,&clip)) &&
           root.paintType!=DWRITE_PAINT_TYPE_NONE && colr_paint_build(program,reader,face,root,clip,input.size);
}
Measurement measure(bool use_native,ColrPaintProgram* program,IDWritePaintReader* reader,IDWriteFontFace* face,
                    const std::vector<Input>& input,std::vector<double>& samples,i64 frequency) {
    Measurement result{};LARGE_INTEGER begin{},end{},a{},b{};ULONG64 cycles_a{},cycles_b{};
    if(!QueryProcessCycleTime(GetCurrentProcess(),&cycles_a))exit(3);
    QueryPerformanceCounter(&begin);
    for(size_t i=0;i<input.size();++i) {
        QueryPerformanceCounter(&a);
        if(!build(use_native,program,reader,face,input[i])) {
            fprintf(stderr,"%s build failed: glyph=%u em=%g\n",use_native?"native":"existing",input[i].glyph,input[i].size);
            exit(2);
        }
        QueryPerformanceCounter(&b);
        samples[i]=(b.QuadPart-a.QuadPart)*1e6/frequency;
        result.bytes+=program->storage.offset;
    }
    QueryPerformanceCounter(&end);
    if(!QueryProcessCycleTime(GetCurrentProcess(),&cycles_b))exit(3);
    result.seconds=static_cast<double>(end.QuadPart-begin.QuadPart)/frequency;
    result.cycles=cycles_b-cycles_a;
    std::sort(samples.begin(),samples.end());
    result.p50_us=samples[(samples.size()-1)*50/100];
    result.p95_us=samples[(samples.size()-1)*95/100];
    result.p99_us=samples[(samples.size()-1)*99/100];
    return result;
}
}
int main(int argc,char** argv) {
    if(argc!=5){fprintf(stderr,"Usage: ColrBuildBenchmark font.ttf glyphs.txt rounds results.csv\n");return 1;}
    char* end=nullptr;const unsigned rounds=static_cast<unsigned>(strtoul(argv[3],&end,10));
    if(!end || *end || rounds<2 || rounds>100)return 1;
    wchar_t path[32768];
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,argv[1],-1,path,32768))return 1;
    FILE* corpus=fopen(argv[2],"r");if(!corpus)return 1;
    std::vector<Input> input;unsigned glyph{},em{};
    while(fscanf(corpus,"%u %u",&glyph,&em)==2) {
        if(glyph>65535 || !em || em>256)return 1;
        input.push_back({static_cast<u16>(glyph),static_cast<float>(em)});
    }
    if(!feof(corpus) || ferror(corpus) || input.empty())return 1;
    fclose(corpus);
    IDWriteFactory* factory=nullptr;
    check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(&factory)),"factory");
    IDWriteFontFile* file=nullptr;IDWriteFontFace* face=nullptr;IDWriteFontFace7* face7=nullptr;IDWritePaintReader* reader=nullptr;
    check(factory->CreateFontFileReference(path,nullptr,&file),"font file");
    check(factory->CreateFontFace(DWRITE_FONT_FACE_TYPE_TRUETYPE,1,&file,0,DWRITE_FONT_SIMULATIONS_NONE,&face),"font face");
    check(face->QueryInterface(__uuidof(IDWriteFontFace7),reinterpret_cast<void**>(&face7)),"font face7");
    check(face7->CreatePaintReader(DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE,DWRITE_PAINT_FEATURE_LEVEL_COLR_V1,&reader),"paint reader");
    check(reader->SetTextColor({1,1,1,1}),"foreground");
    if(!native.open(path)){fprintf(stderr,"Native font preparation failed\n");native.close();return 1;}
    ColrPaintProgram programs[2]{};
    // Each provider sees the same complete warmup and keeps its own arena.
    // Font creation/mapping, scratch allocation and warmup are outside timing.
    for(bool mode:{false,true})for(const auto item:input)
        if(!build(mode,&programs[mode],reader,face,item))return 2;
    std::vector<double> samples(input.size());
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    FILE* output=fopen(argv[4],"w");if(!output)return 1;
    fprintf(output,"round,order,provider,glyphs,seconds,process_cycles,program_bytes,p50_us,p95_us,p99_us\n");
    for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<2;++order) {
        const bool mode=(round+order)%2!=0;
        const auto result=measure(mode,&programs[mode],reader,face,input,samples,frequency.QuadPart);
        fprintf(output,"%u,%u,%s,%zu,%.9f,%llu,%llu,%.3f,%.3f,%.3f\n",round,order,mode?"native":"existing",input.size(),
                result.seconds,result.cycles,result.bytes,result.p50_us,result.p95_us,result.p99_us);
        printf("round=%u provider=%s seconds=%.6f p95_us=%.3f\n",round,mode?"native":"existing",result.seconds,result.p95_us);
        fflush(stdout);
    }
    if(ferror(output) || fclose(output)!=0)return 1;
    for(auto& program:programs)arena_release(&program.storage);
    native.close();reader->Release();face7->Release();face->Release();file->Release();factory->Release();
    return 0;
}
