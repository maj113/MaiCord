#include "../../support/script_owned_units.inc"
bool owned_run(const wchar_t* path,Arena& a,SfntShapeStorage s) noexcept {
    ScriptShapeOwner owner;CHECK(owner.open(path));
    return owned_script_units({NativeClusterScript::sinhala,"Sinhala",0xd9a,0xddd,3,false,L"\\Fonts\\segoeui.ttf",nullptr},owner,a,s);
}
