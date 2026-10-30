#include "maicord/text/emoji_line.hpp"
#include "maicord/core/memory.hpp"
#include "unicode_line_geometry.hpp"
namespace maicord {
SfntStatus native_emoji_line(const NativeRunPlanStore& store,const NativeEmojiPlan& plan,Utf8View text,float em,
    const NativeAsciiLineStorage& s,NativeAsciiLineResult* output,SfntShapeLimits limits,bool coverage,bool rtl,bool arabic,bool paragraph_only) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    NativeAsciiLineResult line{};const auto status=native_emoji_run(store,plan,text,em,s.run,&line.run,limits,coverage,rtl,arabic);if(status!=SfntStatus::ok)return status;
    return native_finish_line(store,plan.run,text,em,s,line,output,limits,0,false,paragraph_only?NativeLineText::paragraph:NativeLineText::unicode);
}
}
