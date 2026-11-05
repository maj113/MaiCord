// The process entry of Linux images. The kernel starts the image at _start; ld.so has already loaded libc.so.6
// (and the pinned libdl.so.2 and libpthread.so.0), relocated the image and initialized libc's thread control block,
// so libc functions (dlopen, pthread_create, clock_gettime) work although __libc_start_main never runs. _start reads
// the initial stack (argc, argv, envp, auxv), records it and calls the image's entry, which ends the process with
// exit_group (on musl through its __libc_start_main, below). Static constructors do not run, as on Windows without
// the CRT.
//
// The image's relative relocations are packed as RELR (ld.lld --pack-dyn-relocs=relr --use-android-relr-tags: a few
// hundred bytes where the RELA table was 70 KB). ld.so leaves them alone: glibc's own DT_RELR needs a loader of 2.36
// or later and a symbol-version dependency that older loaders (2.35 of Ubuntu 22.04, 2.34 of Red Hat 9, 2.31 of
// Debian 11) cannot satisfy, while the Android tags are unknown to every glibc and so ignored. maicord_start applies
// them first of all, before anything reads a pointer from the image's data.
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/libc.hpp"
#include "maicord/sys/linux/process.hpp"

// The image's entry (maicord_program ENTRY): the linker aliases it to this name.
extern "C" [[noreturn]] void maicord_program_entry() noexcept;

// Where the linker maps the image's own ELF header.
extern "C" const maicord::u8 __ehdr_start[] __attribute__((visibility("hidden")));
namespace maicord {
namespace {
ProcessStartup g_startup{0, nullptr, nullptr, nullptr, 4096};
bool g_libc_is_musl;

constexpr u32 kProgramHeaderSelf = 6, kProgramHeaderDynamic = 2, kProgramHeaderRelro = 0x6474E552;
// DT_ANDROID_RELR and DT_ANDROID_RELRSZ.
constexpr i64 kDynamicRelr = 0x6FFFE000, kDynamicRelrSize = 0x6FFFE001;

struct ProgramHeader {
    u32 type, flags;
    u64 offset, address, physical, file_size, memory_size, alignment;
};
// The fields of the ELF header that locate the program headers.
struct ElfHeader {
    u8 identification[16];
    u16 type, machine;
    u32 version;
    u64 entry, program_offset, section_offset;
    u32 flags;
    u16 header_size, program_size, program_count;
};
struct DynamicEntry {
    i64 tag;
    u64 value;
};

// Applies the packed relative relocations. The program headers are found through the ELF header (__ehdr_start, which
// the linker defines where the header is mapped): the auxiliary vector's AT_PHDR names the loader's own headers when
// the image is started as `ld-musl-x86_64.so.1 image`, as musl (unlike glibc) leaves the vector as the kernel built
// it. The PT_PHDR entry gives the link-time address, hence the load bias. The RELRO range is read-only by now (the
// loader protected it after its own relocation pass, which did not include these): it is opened for the pass and
// closed again. Anything unexpected ends the process, since an unrelocated image cannot run.
void apply_packed_relocations() noexcept {
    const auto* elf = reinterpret_cast<const ElfHeader*>(__ehdr_start);
    const u64 headers = reinterpret_cast<u64>(__ehdr_start) + elf->program_offset;
    const u64 entry_size = elf->program_size;
    const u64 count = elf->program_count;
    if (entry_size < sizeof(ProgramHeader)) {
        return;
    }
    u64 bias = 0;
    bool known = false;
    const ProgramHeader* dynamic = nullptr;
    const ProgramHeader* relro = nullptr;
    for (u64 index = 0; index < count; ++index) {
        const auto* header = reinterpret_cast<const ProgramHeader*>(headers + index * entry_size);
        if (header->type == kProgramHeaderSelf) {
            bias = headers - header->address;
            known = true;
        } else if (header->type == kProgramHeaderDynamic) {
            dynamic = header;
        } else if (header->type == kProgramHeaderRelro) {
            relro = header;
        }
    }
    if (!known || !dynamic) {
        return;
    }
    u64 relr = 0, relr_bytes = 0;
    for (const auto* entry = reinterpret_cast<const DynamicEntry*>(bias + dynamic->address); entry->tag != 0; ++entry) {
        if (entry->tag == kDynamicRelr) {
            relr = entry->value;
        } else if (entry->tag == kDynamicRelrSize) {
            relr_bytes = entry->value;
        }
    }
    if (relr == 0 || relr_bytes == 0) {
        return; // no relative relocations at all
    }
    const u64 page_mask = ~(g_startup.page_size - 1);
    void* protected_start = nullptr;
    usize protected_bytes = 0;
    if (relro && relro->memory_size != 0) {
        const u64 start = (bias + relro->address) & page_mask;
        const u64 end = (bias + relro->address + relro->memory_size) & page_mask;
        if (end > start) {
            protected_start = reinterpret_cast<void*>(start);
            protected_bytes = static_cast<usize>(end - start);
            if (syscall_failed(sys_mprotect(protected_start, protected_bytes, kProtectRead | kProtectWrite))) {
                sys_exit_group(127);
            }
        }
    }
    // An even entry is the address of a word to relocate; an odd one is a bitmap of the 63 words after the last.
    u64* where = nullptr;
    const u64* words = reinterpret_cast<const u64*>(bias + relr);
    for (u64 index = 0; index < relr_bytes / sizeof(u64); ++index) {
        const u64 word = words[index];
        if ((word & 1) == 0) {
            where = reinterpret_cast<u64*>(bias + word);
            *where++ += bias;
        } else {
            u64 bits = word >> 1;
            for (u64 at = 0; bits != 0; ++at, bits >>= 1) {
                if (bits & 1) {
                    where[at] += bias;
                }
            }
            where += 63;
        }
    }
    if (protected_start && syscall_failed(sys_mprotect(protected_start, protected_bytes, kProtectRead))) {
        sys_exit_group(127);
    }
}
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

bool process_libc_is_musl() noexcept {
    return g_libc_is_musl;
}

} // namespace maicord

// musl runs the program's start through __libc_start_main, which is what its crt1 does for every dynamic program and
// what its loader leaves undone: __progname and program_invocation_name (Mesa takes the process name from them and
// strrchr()s it), the stack protector's canary and the threads' bookkeeping. glibc's loader has done all of that
// before _start. The libc is told apart by gnu_get_libc_version, which only glibc defines; the functions are found by
// name, so the image's imports stay the audited list.
extern "C" [[noreturn]] int maicord_musl_main(int, char**, char**) noexcept {
    maicord_program_entry();
}

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
    apply_packed_relocations();
    g_libc_is_musl = !dlsym(nullptr, "gnu_get_libc_version");
    if (g_libc_is_musl) {
        using LibcStart = int (*)(int (*)(int, char**, char**), int, char**, void (*)(), void (*)(), void (*)());
        if (const auto start = reinterpret_cast<LibcStart>(dlsym(nullptr, "__libc_start_main"))) {
            start(maicord_musl_main, argc, argv, nullptr, nullptr, nullptr);
        }
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
