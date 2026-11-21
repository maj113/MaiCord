#include "maicord/text/unicode/script.hpp"
#include "maicord/text/unicode/unicode.hpp"
namespace maicord {
// The Unicode Script property (UAX #24), with Common and Inherited resolved from context.
NativeScriptClass native_script_class(u32 cp) noexcept {
    if(!unicode_is_scalar(cp))return {UnicodeScript::zzzz,native_script_invalid};
    return native_script_class(cp,*unicode_properties(cp));
}
void native_script_resolve(Slice<NativeScriptClass> classes) noexcept {
    u32 pending=0;UnicodeScript script=UnicodeScript::zzzz;bool anchored=false;
    // A fill writes only cells before `end`, which the loop has read.
    const auto fill=[&](u32 first,u32 end,NativeScriptClass value) noexcept {for(u32 i=first;i<end;++i)classes[i]=value;};
    for(u32 at=0;at<classes.count;){auto item=classes[at];u32 end=at+1;
        if(end<classes.count&&(classes[end].flags&native_script_selector))++end;
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
    if(!anchored)fill(pending,classes.count,{UnicodeScript::zzzz,0});
}
}
