// Shared helpers for the Khmer/Myanmar/Sinhala/Tibetan shape tests. Include
// inside the test's anonymous namespace before its fail() and CHECK.
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&done,nullptr);}
void number(u64 v) noexcept {char b[32];u32 n=0;do{b[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);while(n){char s[]{b[--n],0};print(s);}}
template<class T> Slice<T> allocate(Arena& a,u32 n) noexcept {return {arena_allocate<T>(&a,n),n};}
SfntShapeStorage storage(Arena& a) noexcept {
    return {allocate<SfntComponentGlyph>(a,512),allocate<SfntComponentNode>(a,1024),allocate<u64>(a,8),
        allocate<u32>(a,512),allocate<u32>(a,1024),allocate<SfntMarkAssociation>(a,512),allocate<double>(a,512),
        allocate<u8>(a,512),allocate<SfntPosition>(a,512),allocate<SfntPlacement>(a,512),
        {allocate<SfntGsubExecutionFrame>(a,32),allocate<u32>(a,512),allocate<u32>(a,512),allocate<u32>(a,512),
         allocate<u8>(a,512*sizeof(SfntComponentGlyph)),allocate<SfntComponentUndo>(a,2048)},
        {allocate<SfntGposExecutionFrame>(a,32),allocate<u32>(a,512),allocate<u64>(a,8),allocate<double>(a,512),allocate<SfntCursiveChainNode>(a,512)}};
}
