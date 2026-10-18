#pragma once

// Compile-time comparisons for the generated SDK conformance units. Names come
// from __FUNCSIG__, which spells types in full (tags, cv, calling convention)
// and a pointer to a virtual member as its vcall thunk, `vcall'{offset}'.
namespace maicord_sys_check {

template <class T> constexpr const char* type_name() {
    return __FUNCSIG__;
}

template <auto Member> constexpr const char* member_name() {
    return __FUNCSIG__;
}

constexpr const char kMirror[] = "maicord_sdk_mirror::";

constexpr bool mirror_prefix_at(const char* text) {
    for (int i = 0; kMirror[i]; ++i) {
        if (text[i] != kMirror[i]) {
            return false;
        }
    }
    return true;
}

// Equal once the mirror namespace is removed from both spellings.
constexpr bool same_text(const char* a, const char* b) {
    for (;;) {
        while (mirror_prefix_at(a)) {
            a += sizeof(kMirror) - 1;
        }
        while (mirror_prefix_at(b)) {
            b += sizeof(kMirror) - 1;
        }
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

// The vtable byte offset of a pointer to a virtual member function, or -1.
constexpr int vtable_offset(const char* name) {
    constexpr const char marker[] = "vcall'{";
    for (; *name; ++name) {
        int i = 0;
        while (marker[i] && name[i] == marker[i]) {
            ++i;
        }
        if (!marker[i]) {
            int value = 0;
            for (name += i; *name >= '0' && *name <= '9'; ++name) {
                value = value * 10 + (*name - '0');
            }
            return value;
        }
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

template <class A, class B> constexpr bool same_uuid() {
    constexpr auto a = __uuidof(A);
    constexpr auto b = __uuidof(B);
    for (int i = 0; i < 8; ++i) {
        if (a.Data4[i] != b.Data4[i]) {
            return false;
        }
    }
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3;
}

} // namespace maicord_sys_check
