#include "maicord/app/login_mfa_policy.hpp"
#include "maicord/core/ascii.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr UserAuthMfaType kLoginMfaOrder[kLoginMfaMethodCount] = {
    UserAuthMfaType::webauthn,
    UserAuthMfaType::totp,
    UserAuthMfaType::backup,
    UserAuthMfaType::sms,
};

LoginMfaCodeResult code_failure(LoginMfaCodeError error) noexcept {
    return LoginMfaCodeResult{0, error};
}

bool printable_ascii(u8 value) noexcept {
    return value >= 0x20u && value <= 0x7Eu;
}

} // namespace

u8 login_mfa_capability(UserAuthMfaType type) noexcept {
    switch (type) {
    case UserAuthMfaType::totp:
        return user_auth_mfa_capability_totp;
    case UserAuthMfaType::sms:
        return user_auth_mfa_capability_sms;
    case UserAuthMfaType::backup:
        return user_auth_mfa_capability_backup;
    case UserAuthMfaType::webauthn:
        return user_auth_mfa_capability_webauthn;
    default:
        return user_auth_mfa_capability_none;
    }
}

bool login_mfa_first_supported(u8 capabilities, UserAuthMfaType* type) noexcept {
    if (!type) {
        return false;
    }
    for (u32 index = 0; index < kLoginMfaMethodCount; ++index) {
        const UserAuthMfaType candidate = kLoginMfaOrder[index];
        if ((capabilities & login_mfa_capability(candidate)) != 0) {
            *type = candidate;
            return true;
        }
    }
    return false;
}

bool login_mfa_invalid_error(u64 error_code) noexcept {
    return error_code == kLoginMfaInvalidCode;
}

u32 login_mfa_backup_acknowledgement_seconds(u64 now_ms, u64 ready_at_ms) noexcept {
    if (ready_at_ms == 0 || now_ms >= ready_at_ms) {
        return 0;
    }
    const u64 remaining = ready_at_ms - now_ms;
    return static_cast<u32>(minimum<u64>((remaining + 999) / 1'000, 10));
}

LoginMfaCodeResult login_mfa_validate_code(UserAuthMfaType type, ConstSlice<u8> input) noexcept {
    if (!input.data || input.count == 0) {
        return code_failure(LoginMfaCodeError::invalid_input);
    }
    if (type == UserAuthMfaType::totp || type == UserAuthMfaType::sms) {
        if (input.count != kLoginMfaCodeDigits) {
            return code_failure(LoginMfaCodeError::invalid_length);
        }
        for (u32 index = 0; index < input.count; ++index) {
            if (!ascii_is_digit(input.data[index])) {
                return code_failure(LoginMfaCodeError::invalid_character);
            }
        }
        return LoginMfaCodeResult{input.count, LoginMfaCodeError::none};
    }
    if (type != UserAuthMfaType::backup) {
        return code_failure(LoginMfaCodeError::invalid_type);
    }
    if (input.count < kLoginMfaBackupMinimumCharacters || input.count > kLoginMfaBackupMaximumCharacters) {
        return code_failure(LoginMfaCodeError::invalid_length);
    }
    u32 normalized_bytes = 0;
    for (u32 index = 0; index < input.count; ++index) {
        const u8 value = input.data[index];
        if (!printable_ascii(value)) {
            return code_failure(LoginMfaCodeError::invalid_character);
        }
        normalized_bytes += value != static_cast<u8>('-');
    }
    if (normalized_bytes == 0) {
        return code_failure(LoginMfaCodeError::invalid_character);
    }
    return LoginMfaCodeResult{normalized_bytes, LoginMfaCodeError::none};
}

LoginMfaCodeResult login_mfa_prepare_code(UserAuthMfaType type, ConstSlice<u8> input,
                                          SecureBuffer* destination) noexcept {
    const LoginMfaCodeResult validated = login_mfa_validate_code(type, input);
    if (!validated.ok()) {
        return validated;
    }
    if (!destination || !destination->data || !destination->locked ||
        destination->capacity < validated.bytes) {
        return code_failure(destination && destination->data && destination->locked
                                ? LoginMfaCodeError::output_too_small
                                : LoginMfaCodeError::invalid_input);
    }
    if (type != UserAuthMfaType::backup) {
        const SecureBufferResult assigned = secure_buffer_assign(destination, input);
        return assigned.ok() ? validated : code_failure(LoginMfaCodeError::invalid_input);
    }
    SecureZeroMemory(destination->data, destination->capacity);
    u32 output = 0;
    for (u32 index = 0; index < input.count; ++index) {
        const u8 value = input.data[index];
        if (value != static_cast<u8>('-')) {
            destination->data[output++] = value;
        }
    }
    destination->bytes = output;
    return LoginMfaCodeResult{output, LoginMfaCodeError::none};
}

} // namespace maicord
