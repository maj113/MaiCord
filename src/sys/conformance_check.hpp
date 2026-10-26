#pragma once

// Compile-time comparisons for the generated SDK conformance units. Type names
// come from __FUNCSIG__, which spells the canonical template argument in full
// (scopes, cv, pointers). Member names come from __FUNCDNAME__: the Microsoft
// mangling of a pointer to a virtual member is its vcall thunk, which encodes
// the vtable byte offset (??_9<class>@$B<offset>A<convention>).
namespace maicord_sys_check {

template <class T> constexpr const char* type_name() {
    return __FUNCSIG__;
}

template <auto Member> constexpr const char* member_name() {
    return __FUNCDNAME__;
}

constexpr const char kMirror[] = "maicord_sdk_mirror::";
constexpr const char kNoexcept[] = " noexcept";

constexpr bool prefix_at(const char* text, const char* prefix) {
    for (int i = 0; prefix[i]; ++i) {
        if (text[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

// Past the mirror namespace and noexcept, which MaiCord's declarations add and
// the ABI ignores.
constexpr const char* skip_mirror(const char* text) {
    for (;;) {
        if (prefix_at(text, kMirror)) {
            text += sizeof(kMirror) - 1;
        } else if (prefix_at(text, kNoexcept)) {
            text += sizeof(kNoexcept) - 1;
        } else {
            return text;
        }
    }
}

// Equal once the mirror namespace and noexcept are removed from both spellings.
constexpr bool same_text(const char* a, const char* b) {
    for (;;) {
        a = skip_mirror(a);
        b = skip_mirror(b);
        if (*a != *b) {
            return false;
        }
        if (!*a) {
            return true;
        }
        ++a;
        ++b;
    }
}

template <class A, class B> constexpr bool same_type() {
    return same_text(type_name<A>(), type_name<B>());
}

// The vtable byte offset of a pointer to a virtual member function, or -1. A
// mangled number is 1-10 as one digit 0-9, else hexadecimal in A-P ending '@'.
constexpr int vtable_offset(const char* name) {
    constexpr const char marker[] = "??_9";
    for (; *name; ++name) {
        int i = 0;
        while (marker[i] && name[i] == marker[i]) {
            ++i;
        }
        if (marker[i]) {
            continue;
        }
        for (name += i; *name && !(name[0] == '$' && name[1] == 'B'); ++name) {
        }
        if (!*name) {
            return -1;
        }
        name += 2;
        if (*name >= '0' && *name <= '9') {
            return *name - '0' + 1;
        }
        int value = 0;
        for (; *name >= 'A' && *name <= 'P'; ++name) {
            value = value * 16 + (*name - 'A');
        }
        return *name == '@' ? value : -1;
    }
    return -1;
}

template <auto Member> constexpr int slot() {
    return vtable_offset(member_name<Member>());
}

template <class A, class B> constexpr bool same_guid(const A& a, const B& b) {
    for (int i = 0; i < 8; ++i) {
        if (a.Data4[i] != b.Data4[i]) {
            return false;
        }
    }
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3;
}

// __uuidof names the compiler's GUID object. Its type is _GUID even for a
// dependent operand, which may be incomplete where these templates are
// defined: uuid_of's deduced result defers every member access to the
// instantiation.
template <class T> constexpr const auto& uuid_of() {
    return __uuidof(T);
}

template <class A, class B> constexpr bool same_uuid() {
    return same_guid(uuid_of<A>(), uuid_of<B>());
}

} // namespace maicord_sys_check
