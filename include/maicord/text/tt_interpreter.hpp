#pragma once
#include "maicord/text/sfnt.hpp"
namespace maicord {
struct TtPoint {i32 x,y,original_x,original_y;u8 touched;u8 on_curve;u16 reserved;};
struct TtFunction {ConstSlice<u8> code;bool defined;};
struct TtCallFrame {ConstSlice<u8> code;u32 offset,remaining;};
enum class TtRound : u8 {grid,half_grid,double_grid,down,up,off,super};
enum class TtPhase : u8 {font,prep,glyph};
struct TtGraphicsState {
    i16 projection_x=16384,projection_y=0,freedom_x=16384,freedom_y=0,dual_x=16384,dual_y=0;
    u32 rp[3]{},zp[3]{1,1,1},loop=1;
    i32 minimum_distance=64,cvt_cut_in=68,single_width_cut_in=0,single_width=0;
    u32 delta_base=9,delta_shift=3,instruction_control=0,scan_control=0,scan_type=0;
    TtRound round=TtRound::grid;
    i16 round_period=64,round_phase=0,round_threshold=32;
    bool auto_flip=true;
};
struct TtInterpreter {
    Slice<i32> stack,storage,cvt;
    Slice<TtFunction> functions;
    Slice<TtCallFrame> frames;
    Slice<TtPoint> glyph,twilight;
    ConstSlice<u32> contour_ends;
    TtGraphicsState graphics;
    u32 stack_count=0,units_per_em=0;
};
struct TtExecution {u32 operations,byte_offset,opcode,stack_count,call_depth;bool missing_function;};
// Normalized-outline compatibility profile matching Windows. The point
// arrays and CVT use signed F26Dot6, with one pixel per design unit. Callers provide
// all storage and must keep borrowed FDEF bytecode unchanged. No recursion or allocation.
// Missing opcodes return unsupported with diagnostics; they are never no-ops.
// Execution mutates working state on failure, which must then be discarded.
SfntStatus tt_execute(TtInterpreter* state,ConstSlice<u8> program,TtPhase phase,
    TtExecution* result,u32 work_limit=1000000) noexcept;
} // namespace maicord
