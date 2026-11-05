# Clear URLs

Removes tracking parameters (`utm_*`, `fbclid`, `gclid` and other common ones)
from the links in messages you send, and from edits of your messages.

| Setting | Type | Default |
|---|---|---|
| Also clean edited messages (`on_edit`) | Toggle | on |
| Extra parameters to remove (`extra`) | Text list, one name per line; a trailing `*` matches by prefix | empty |

Permissions: `message_send`, implied by its `pre_send` and `pre_edit` hooks.

`plugin.cpp` is the whole plugin: a settings schema, two hooks and the
descriptor, written with `maicord/sdk/plugin.hpp` (ABI 6). The DLL it builds
(a shared object on Linux)
has no entry point, no TLS, no imports and one export, `maicord_plugin_v6`, a
`const` descriptor, so loading it runs none of its code.

## Building

Inside the MaiCord repository, `cmake/test_registry.cmake` builds it with the
`maicord_plugin` helper from `cmake/test_support.cmake`:

```cmake
maicord_plugin(MaiCordClearUrlsPlugin samples/plugins/clear-urls/plugin.cpp)
set_target_properties(MaiCordClearUrlsPlugin PROPERTIES OUTPUT_NAME clear-urls)
```

On its own, with clang-cl and lld-link (`<include>` is the directory that
contains `maicord/sdk/`):

```powershell
clang-cl /nologo /c /std:c++latest /O2 /W4 /WX /utf-8 /Zl /GS- /GR- /DMC_RUNTIME /I<include> plugin.cpp
lld-link /nologo /DLL /NOENTRY /NODEFAULTLIB /OPT:REF /OPT:ICF plugin.obj /OUT:clear-urls.dll
```

On Linux the same source builds a shared object (`cmake/linux_plugin_tests.cmake` registers
it, and the other samples, the same way; the recipe is in `include/maicord/sdk/README.md`):

```sh
clang++ -std=c++23 -O2 -nostdinc -I<include> -I<include>/maicord/sys/libc -fPIC -fvisibility=hidden \
  -fno-stack-protector -fno-exceptions -fno-rtti -DMC_RUNTIME -c plugin.cpp -o plugin.o
clang++ -fuse-ld=lld -nostdlib -shared -Wl,-Bsymbolic -Wl,-z,noexecstack plugin.o -o clear-urls.so
```

`MC_RUNTIME` makes `plugin.hpp` define the few compiler helpers a CRT would
otherwise provide (`memcpy`, `memset`, `memmove`, `memcmp`, and on Windows
`__chkstk`, `_fltused`); define it in exactly one translation unit.
