#pragma once

// std::initializer_list as the compiler's <initializer_list> declares it: the
// compiler constructs it from a pointer pair; see maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <initializer_list>
#else
namespace std {

template <class _Elem> class initializer_list {
public:
    constexpr initializer_list() noexcept : _First(nullptr), _Last(nullptr) {}
    constexpr initializer_list(const _Elem* _First_arg, const _Elem* _Last_arg) noexcept
        : _First(_First_arg), _Last(_Last_arg) {}

    [[nodiscard]] constexpr const _Elem* begin() const noexcept {
        return _First;
    }
    [[nodiscard]] constexpr const _Elem* end() const noexcept {
        return _Last;
    }

private:
    const _Elem* _First;
    const _Elem* _Last;
};

} // namespace std
#endif
