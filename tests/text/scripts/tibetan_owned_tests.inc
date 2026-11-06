#include "../../support/script_owned_units.inc"
bool owned_stream(const wchar_t* path,Arena& a,SfntShapeStorage s) noexcept {
    ScriptShapeOwner owner;CHECK(owner.open(path));
    return owned_script_units({NativeClusterScript::tibetan,"Tibetan",0xf40,0xf72,2,true,L"\\Fonts\\consola.ttf",nullptr},owner,a,s);
}
