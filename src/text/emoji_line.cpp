#include "maicord/text/emoji_line.hpp"
#include "unicode_line_geometry.hpp"
namespace maicord {
SfntStatus native_emoji_line(const NativeRunPlanStore& store,const NativeEmojiPlan& plan,Utf8View text,float em,
    const NativeAsciiLineStorage& s,NativeAsciiLineResult* output,SfntShapeLimits limits,bool coverage,bool rtl,bool arabic,bool paragraph_only) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};if(s.clusters.count&&!s.clusters.data)return SfntStatus::invalid_input;
    NativeAsciiLineResult line{};auto status=native_emoji_run(store,plan,text,em,s.run,&line.run,limits,coverage,rtl,arabic);if(status!=SfntStatus::ok)return status;
    const auto* metrics=native_run_plan_metrics(store,plan.run);if(!metrics)return SfntStatus::invalid_input;
    status=native_unicode_line_geometry(*metrics,text,em,s,&line,paragraph_only);if(status==SfntStatus::ok)*output=line;return status;
}
}
