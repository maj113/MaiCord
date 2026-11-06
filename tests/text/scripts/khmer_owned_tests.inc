#include "../../support/script_owned_units.inc"
bool owned_units(ScriptShapeOwner& owner,Arena& state,SfntShapeStorage s) noexcept {
    static const u32 generic[]{0x1780,0x17b4,0x17b5,0x17d2,0x179a};
    return owned_script_units({NativeClusterScript::khmer,"Khmer",0x1780,0x17be,3,false,L"\\Fonts\\consola.ttf",generic},owner,state,s);
}
