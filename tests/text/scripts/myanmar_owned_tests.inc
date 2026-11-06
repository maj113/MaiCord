#include "../../support/script_owned_units.inc"
bool owned_stream(const wchar_t* path,Arena& a,SfntShapeStorage s) noexcept {
    ScriptShapeOwner owner;CHECK(owner.open(path));
    return owned_script_units({NativeClusterScript::myanmar,"Myanmar",0x1000,0x1031,2,true,L"\\Fonts\\segoeui.ttf",nullptr},owner,a,s);
}
