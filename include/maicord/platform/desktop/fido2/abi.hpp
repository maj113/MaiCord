#pragma once

#include "maicord/core/base.hpp"

// The libfido2 interface MaiCord uses for a security-key assertion, declared without libfido2's headers (Linux). The
// library is dlopen()ed ("libfido2.so.1"), so a system without it reports security keys as unavailable instead of failing
// to load. tools/check_fido2_abi.py compares this file with the installed libfido2-dev headers (constants marked with
// the macro they copy, the function table). Only functions that exist in libfido2 1.3 (Ubuntu 20.04) are used: the
// authenticator data is read through fido_assert_authdata_ptr (a CBOR byte string; the header is stripped here), not
// through the raw accessors of 1.8.

namespace maicord::fido2 {

struct Device;      // fido_dev_t
struct DeviceInfo;  // fido_dev_info_t
struct Assertion;   // fido_assert_t

inline constexpr i32 kOk = 0x00;                  // FIDO_OK
inline constexpr i32 kErrorTimeout = 0x05;        // FIDO_ERR_TIMEOUT
inline constexpr i32 kErrorKeepaliveCancel = 0x2d; // FIDO_ERR_KEEPALIVE_CANCEL
inline constexpr i32 kErrorNoCredentials = 0x2e;  // FIDO_ERR_NO_CREDENTIALS
inline constexpr i32 kErrorUserActionTimeout = 0x2f; // FIDO_ERR_USER_ACTION_TIMEOUT
inline constexpr i32 kErrorPinInvalid = 0x31;     // FIDO_ERR_PIN_INVALID
inline constexpr i32 kErrorPinBlocked = 0x32;     // FIDO_ERR_PIN_BLOCKED
inline constexpr i32 kErrorPinAuthInvalid = 0x33; // FIDO_ERR_PIN_AUTH_INVALID
inline constexpr i32 kErrorPinAuthBlocked = 0x34; // FIDO_ERR_PIN_AUTH_BLOCKED
inline constexpr i32 kErrorPinNotSet = 0x35;      // FIDO_ERR_PIN_NOT_SET
inline constexpr i32 kErrorPinRequired = 0x36;    // FIDO_ERR_PIN_REQUIRED
inline constexpr i32 kErrorActionTimeout = 0x3a;  // FIDO_ERR_ACTION_TIMEOUT
inline constexpr i32 kErrorUvBlocked = 0x3c;      // FIDO_ERR_UV_BLOCKED
inline constexpr i32 kErrorTransmit = -1;         // FIDO_ERR_TX
inline constexpr i32 kErrorInternal = -9;         // FIDO_ERR_INTERNAL

// fido_opt_t: what an assertion asks of an authenticator option (user verification here).
inline constexpr i32 kOptionOmit = 0;  // FIDO_OPT_OMIT
inline constexpr i32 kOptionFalse = 1; // FIDO_OPT_FALSE
inline constexpr i32 kOptionTrue = 2;  // FIDO_OPT_TRUE

// libfido2's functions. fido_dev_info_manifest lists the HID devices (hidraw on Linux); fido_dev_get_assert blocks until the
// key answers (a touch, a PIN prompt on the key, a timeout) and may be interrupted from another thread by fido_dev_cancel.
#define MAICORD_FIDO2_FUNCTIONS(F)                                                                                     \
    F(void, fido_init, (i32 flags))                                                                                    \
    F(DeviceInfo*, fido_dev_info_new, (usize count))                                                                   \
    F(void, fido_dev_info_free, (DeviceInfo * *list, usize count))                                                     \
    F(i32, fido_dev_info_manifest, (DeviceInfo * list, usize capacity, usize* found))                                  \
    F(const DeviceInfo*, fido_dev_info_ptr, (const DeviceInfo* list, usize index))                                     \
    F(const char*, fido_dev_info_path, (const DeviceInfo* info))                                                       \
    F(Device*, fido_dev_new, ())                                                                                       \
    F(void, fido_dev_free, (Device * *device))                                                                         \
    F(i32, fido_dev_open, (Device * device, const char* path))                                                         \
    F(i32, fido_dev_close, (Device * device))                                                                          \
    F(i32, fido_dev_cancel, (Device * device))                                                                         \
    F(i32, fido_dev_get_retry_count, (Device * device, i32* retries))                                                  \
    F(i32, fido_dev_get_assert, (Device * device, Assertion* assertion, const char* pin))                              \
    F(Assertion*, fido_assert_new, ())                                                                                 \
    F(void, fido_assert_free, (Assertion * *assertion))                                                                \
    F(i32, fido_assert_set_rp, (Assertion * assertion, const char* rp_id))                                             \
    F(i32, fido_assert_set_clientdata_hash, (Assertion * assertion, const u8* hash, usize bytes))                      \
    F(i32, fido_assert_allow_cred, (Assertion * assertion, const u8* id, usize bytes))                                 \
    F(i32, fido_assert_set_uv, (Assertion * assertion, i32 option))                                                    \
    F(usize, fido_assert_count, (const Assertion* assertion))                                                          \
    F(const u8*, fido_assert_id_ptr, (const Assertion* assertion, usize index))                                        \
    F(usize, fido_assert_id_len, (const Assertion* assertion, usize index))                                            \
    F(const u8*, fido_assert_authdata_ptr, (const Assertion* assertion, usize index))                                  \
    F(usize, fido_assert_authdata_len, (const Assertion* assertion, usize index))                                      \
    F(const u8*, fido_assert_sig_ptr, (const Assertion* assertion, usize index))                                       \
    F(usize, fido_assert_sig_len, (const Assertion* assertion, usize index))                                           \
    F(const u8*, fido_assert_user_id_ptr, (const Assertion* assertion, usize index))                                   \
    F(usize, fido_assert_user_id_len, (const Assertion* assertion, usize index))

struct Api {
#define MAICORD_FIDO2_MEMBER(result, name, arguments) result(*name) arguments;
    MAICORD_FIDO2_FUNCTIONS(MAICORD_FIDO2_MEMBER)
#undef MAICORD_FIDO2_MEMBER
};

} // namespace maicord::fido2
