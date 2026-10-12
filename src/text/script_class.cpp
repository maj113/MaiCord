#include "maicord/text/script.hpp"
#include "maicord/text/visibility.hpp"
namespace maicord {namespace {
struct ScriptOverride {u32 first,last_script;};
#include "generated/script_compat_data.inc"
}
NativeScriptClass native_script_class(u32 cp) noexcept {
    if(!unicode_is_scalar(cp))return {UnicodeScript::zzzz,native_script_invalid};
    if(native_text_nonvisual(cp))return {UnicodeScript::zyyy,native_script_nonvisual};
    if(cp>=script_overrides[0].first){
        u32 lo=0,hi=sizeof(script_overrides)/sizeof(script_overrides[0]);
        while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(script_overrides[mid].first<=cp)lo=mid+1;else hi=mid;}
        if(lo){const u32 value=script_overrides[lo-1].last_script;
            if(cp<=(value&0x1fffffu))return {static_cast<UnicodeScript>(value>>21),0};}
    }
    const auto* properties=unicode_properties(cp);const auto script=properties->script();
    const u8 selector=properties->has(UnicodeFlag::variation_selector)?native_script_selector:0;
    if(script==UnicodeScript::zyyy||script==UnicodeScript::zinh)return {UnicodeScript::zzzz,static_cast<u8>(native_script_contextual|selector)};
    return {script,static_cast<u8>(selector?native_script_contextual|selector:0)};
}
NativeScriptStatus native_script_resolve(ConstSlice<u32> scalars,Slice<NativeScriptClass> output) noexcept {
    if((scalars.count&&!scalars.data)||(output.count&&!output.data))return NativeScriptStatus::invalid_input;
    if(output.count<scalars.count)return NativeScriptStatus::output_too_small;
    u32 pending=0;UnicodeScript script=UnicodeScript::zzzz;bool anchored=false,have_next=false;NativeScriptClass next{};
    const auto fill=[&](u32 first,u32 end,NativeScriptClass value) noexcept {for(u32 i=first;i<end;++i)output[i]=value;};
    for(u32 at=0;at<scalars.count;){auto item=have_next?next:native_script_class(scalars[at]);have_next=false;
        if(item.flags&native_script_invalid)return NativeScriptStatus::invalid_input;u32 end=at+1;
        if(end<scalars.count){next=native_script_class(scalars[end]);if(next.flags&native_script_invalid)return NativeScriptStatus::invalid_input;
            if(next.flags&native_script_selector)++end;else have_next=true;}
        // An unpaired Mongolian selector retains its own script. Ordinary
        // variation selectors retain the neutral isolated-input fallback.
        if((item.flags&native_script_selector)&&item.script!=UnicodeScript::zzzz)item.flags&=static_cast<u8>(~native_script_contextual);
        if(item.flags&native_script_nonvisual){
            if(!anchored)fill(pending,at,{UnicodeScript::zzzz,0});
            fill(at,end,{UnicodeScript::zyyy,native_script_nonvisual});pending=end;anchored=false;
        }else{
            if(!(item.flags&native_script_contextual)){script=item.script;
                if(!anchored){fill(pending,at,{script,0});anchored=true;}}
            if(anchored)fill(at,end,{script,0});
        }
        at=end;
    }
    if(!anchored)fill(pending,scalars.count,{UnicodeScript::zzzz,0});return NativeScriptStatus::ok;
}
}
