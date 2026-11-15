// Storage for one ASCII or script run, shared by the run plan and emoji tests.
struct AsciiBuffers {
    SfntClusterGlyph mapped[256];u16 glyphs[1024];SfntPlacement placements[1024];u32 map[256];
    SfntComponentGlyph records[1024],replacements[1024];SfntComponentNode nodes[256];SfntComponentUndo journal[4096];
    u64 bits[8],cursive_bits[16];u32 characters[256],targets[1024],positions[4096],rebased[4096],consumed[1024],gp_positions[4096];
    SfntMarkAssociation associations[1024];double advances[1024],prefix[1024];u8 flags[1024];SfntPosition placed[1024];SfntPlacement output[1024];
    SfntGsubExecutionFrame frames[64];SfntGposExecutionFrame gp_frames[64];SfntCursiveChainNode chains[1024];
    NativeAsciiStorage storage() noexcept {return {{mapped,256},
        {{records,1024},{nodes,256},{bits,8},{characters,256},{targets,1024},{associations,1024},{advances,1024},{flags,1024},{placed,1024},{output,1024},
         {{frames,64},{positions,4096},{rebased,4096},{consumed,1024},{reinterpret_cast<u8*>(replacements),sizeof(replacements)},{journal,4096}},
         {{gp_frames,64},{gp_positions,4096},{cursive_bits,16},{prefix,1024},{chains,1024}}},
        {glyphs,1024},{placements,1024},{map,256}};}
    NativeAsciiStorage shared() noexcept {auto result=storage();result.mapped={reinterpret_cast<SfntClusterGlyph*>(result.shaping.positions.data),256};result.glyphs={reinterpret_cast<u16*>(result.shaping.glyphs.data),1024};result.placements=result.shaping.placements;result.character_glyphs=result.shaping.character_glyphs;return result;}
} ascii_buffers;
