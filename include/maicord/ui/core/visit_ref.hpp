#pragma once

namespace maicord {

// Non-owning type-erased predicate over Args. Walkers that take one compile
// once however many callers visit; the visitor must outlive the call.
template <class... Args> struct UiVisitRef {
    const void* context;
    bool (*call)(const void*, Args...);
    template <class F> UiVisitRef(const F& visitor) noexcept
        : context(&visitor), call([](const void* c, Args... arguments) noexcept {
              return (*static_cast<const F*>(c))(arguments...);
          }) {}
    bool operator()(Args... arguments) const noexcept { return call(context, arguments...); }
};

} // namespace maicord
