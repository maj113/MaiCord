// Owner of one native font, instance and run plan for the script shape tests.
// Include after the test's CHECK macro.
struct ScriptShapeOwner {
    NativeFontFileSlot files[1]{};NativeFontFaceSlot faces[1]{};NativeFontInstanceSlot instances[1]{};NativeRunPlanSlot plans[1]{};
    NativeFontStore fonts{{files,1},{faces,1}};NativeFontInstanceStore instance_store{&fonts,{instances,1}};NativeRunPlanStore store{&instance_store,{plans,1}};
    NativeFontHandle face{};NativeFontInstanceHandle instance{};NativeRunPlanHandle run{};Arena scratch{};
    ~ScriptShapeOwner() noexcept {if(run.slot)native_run_plan_release(&store,run);if(instance.slot)native_font_instance_release(&instance_store,instance);
        if(face.slot)native_font_release(&fonts,face);arena_release(&scratch);}
    bool open(const wchar_t* path) noexcept {
        CHECK(arena_initialize(&scratch,16*1024*1024,65536));CHECK(native_font_acquire(&fonts,path,0,&face).ok());
        CHECK(native_font_instance_acquire(&instance_store,face,{},false,{},&instance).ok());
        CHECK(native_run_plan_acquire(&store,instance,&scratch,&run).ok());return true;
    }
    bool close() noexcept {
        CHECK(native_run_plan_release(&store,run));run={};CHECK(native_font_instance_release(&instance_store,instance));instance={};
        CHECK(native_font_release(&fonts,face));face={};const auto stats=native_font_stats(fonts);
        CHECK(!stats.files&&!stats.faces&&!stats.references&&!stats.mapped_bytes);return true;
    }
};
