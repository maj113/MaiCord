# The static libraries of the Linux build (cmake/libraries.cmake lists the Windows ones). Each source list is the
# archive's member order; the Linux-only platform files live in src/core/linux.

# The core: memory, byte, UTF-8 and JSON primitives, and the Linux side of the platform files the Windows build
# takes from src/core (data_path, log) and src/core's Win32 sources (src/core/delay_load.cpp, runtime_x64.asm and
# win32_sdk_conformance.cpp have no Linux counterpart).
maicord_library(maicord_core
    src/core/arena.cpp src/core/byte_queue.cpp src/core/byte_order.cpp src/core/linux/data_path.cpp
    src/core/json_reader.cpp src/core/json_scan.cpp src/core/reserved_bytes.cpp src/core/stable_storage.cpp
    src/core/utf.cpp src/core/byte_range.cpp src/core/linux/log.cpp src/core/linux/path.cpp
    src/core/linux/shared_library.cpp)
