#pragma once

#include "../../../src/platform/uia/provider_internal.hpp"
#include "maicord/sys/uia.hpp"

namespace maicord::accessibility_test {
namespace accessibility_detail {

void check(bool valid, const char* name) noexcept;

bool equal(Utf8View text, Utf8View expected) noexcept;

// ---- synthetic scene -----------------------------------------------------

struct Scene {
    UiWidget widgets[32];
    UiLayoutNode layout[32];
    UiWidgetManager manager;
    UiWidgetHandle root, list, rows[3], labels[3], icon, settings, label, field, hidden, footer, disabled;
};

bool build_tree(UiAccessTree* tree, const Scene& scene, bool with_row0 = true) noexcept;

// ---- provider (direct COM calls through the SDK's C++ interfaces) ---------

struct ActRecord {
    u64 key;
    UiAccessAction action;
    double number;
    char text[32];
    u32 text_bytes;
    u32 calls;
    DWORD thread;
};

struct HostContext {
    UiAccessTree* tree;
    ActRecord act;
    DWORD tree_thread;
};

const UiAccessTree* host_tree(void* context) noexcept;

bool host_act(void* context, u64 key, UiAccessAction action, double number, Utf8View text) noexcept;

float host_scale(void*) noexcept;

IRawElementProviderSimple* element(UiaProviderHost* host, u64 key) noexcept;

template <typename T> T* pattern(IRawElementProviderSimple* simple, PATTERNID id) noexcept {
    IUnknown* unknown = nullptr;
    if (!simple || FAILED(simple->GetPatternProvider(id, &unknown)) || !unknown)
        return nullptr;
    T* result = nullptr;
    unknown->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&result));
    unknown->Release();
    return result;
}

int int_property(IRawElementProviderSimple* simple, PROPERTYID id) noexcept;

bool has_property(const UiAccessEventPlan& plan, const UiAccessTree& tree, u64 key, UiAccessProperty property) noexcept;

void test_events(UiAccessTree* tree) noexcept;

void test_overlays() noexcept;

void test_exhaustion(const Scene& scene) noexcept;

void test_text_units() noexcept;

} // namespace accessibility_detail

using namespace accessibility_detail;
} // namespace maicord::accessibility_test
