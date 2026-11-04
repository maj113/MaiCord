// The process entry of Linux images. The kernel starts the image at _start; ld.so has already loaded libc.so.6
// (and the pinned libdl.so.2 and libpthread.so.0), relocated the image and initialized libc's thread control block,
// so libc functions (dlopen, pthread_create, clock_gettime) work although __libc_start_main never runs. _start reads
// the initial stack (argc, argv, envp, auxv), records it and calls the image's entry, which ends the process with
// exit_group. Static constructors do not run, as on Windows without the CRT.
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"

// The image's entry (maicord_program ENTRY): the linker aliases it to this name.
extern "C" [[noreturn]] void maicord_program_entry() noexcept;

namespace maicord {
namespace {
ProcessStartup g_startup{0, nullptr, nullptr, nullptr, 4096};
} // namespace

const ProcessStartup& process_startup() noexcept {
    return g_startup;
}

const char* environment_get(const char* name) noexcept {
    if (!g_startup.environment) {
        return nullptr;
    }
    for (char** entry = g_startup.environment; *entry; ++entry) {
        const char* text = *entry;
        const char* wanted = name;
        while (*wanted && *text == *wanted) {
            ++text;
            ++wanted;
        }
        if (*wanted == 0 && *text == '=') {
            return text + 1;
        }
    }
    return nullptr;
}

u64 auxiliary_value(u64 type) noexcept {
    if (g_startup.auxiliary) {
        for (const AuxEntry* entry = g_startup.auxiliary; entry->type != kAuxNull; ++entry) {
            if (entry->type == type) {
                return entry->value;
            }
        }
    }
    return 0;
}

} // namespace maicord

// The stack pointer at _start points at argc, then argv[argc], a null, envp, a null and the auxiliary vector.
extern "C" [[noreturn, gnu::used]] void maicord_start(maicord::usize* stack) noexcept {
    using namespace maicord;
    const int argc = static_cast<int>(stack[0]);
    char** argv = reinterpret_cast<char**>(stack + 1);
    char** environment = argv + argc + 1;
    char** end = environment;
    while (*end) {
        ++end;
    }
    g_startup.argc = argc;
    g_startup.argv = argv;
    g_startup.environment = environment;
    g_startup.auxiliary = reinterpret_cast<const AuxEntry*>(end + 1);
    const u64 page_size = auxiliary_value(kAuxPageSize);
    if (page_size != 0) {
        g_startup.page_size = page_size;
    }
    maicord_program_entry();
    sys_exit_group(0);
}

// xor %ebp: the outermost frame. The stack is aligned for the call; rdx (the dynamic linker's exit hook) is unused.
__asm__(".text\n"
        ".globl _start\n"
        ".type _start,@function\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    mov %rsp, %rdi\n"
        "    and $-16, %rsp\n"
        "    call maicord_start\n"
        "    ud2\n"
        ".size _start, .-_start\n");
