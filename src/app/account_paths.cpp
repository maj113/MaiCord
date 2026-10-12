#include "maicord/core/win32_process.hpp"
#include "maicord/app/account_paths.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

namespace maicord {
namespace {

constexpr u32 kCapacity = kAccountProfileMaximumPathCharacters;

AccountPathsResult failure(AccountPathsError error, u32 native_error = 0) noexcept {
    return AccountPathsResult{native_error, error};
}

u32 g_selected_slot = 0;

} // namespace

AccountPathsResult account_paths_build_slot(const wchar_t* local_application_data, u32 slot,
                                            AccountPaths* paths) noexcept {
    if (!local_application_data || local_application_data[0] == 0 || !paths || slot >= kAccountPathsSlotCount)
        return failure(AccountPathsError::invalid_input);
    wchar_t accounts[16] = L"accounts";
    u32 accounts_length = 8;
    const wchar_t slot_directory[] = {L'\\', L's', L'l', L'o', L't', L'-', static_cast<wchar_t>(L'0' + slot), 0};
    if (slot != 0)
        wide_append(accounts, 16, &accounts_length, slot_directory);
    AccountPaths& built = *paths;
    wchar_t* outputs[] = {built.application_directory, built.account_directory, built.profile, built.session,
                          built.drafts};
    const wchar_t* files[] = {L"\\active.profile", L"\\active.session", L"\\active.drafts"};
    bool complete = data_path_build(local_application_data, built.application_directory, kCapacity, nullptr) &&
                    data_path_build(local_application_data, built.account_directory, kCapacity, accounts);
    for (u32 index = 0; complete && index < 3; ++index) {
        wchar_t relative[32];
        u32 length = 0;
        complete = wide_append(relative, 32, &length, accounts) && wide_append(relative, 32, &length, files[index]) &&
                   data_path_build(local_application_data, outputs[2 + index], kCapacity, relative);
    }
    if (!complete) {
        for (wchar_t* output : outputs)
            output[0] = 0;
        return failure(AccountPathsError::capacity);
    }
    return AccountPathsResult{};
}

bool account_paths_select_slot(u32 slot) noexcept {
    if (slot >= kAccountPathsSlotCount)
        return false;
    g_selected_slot = slot;
    return true;
}

u32 account_paths_selected_slot() noexcept {
    return g_selected_slot;
}

AccountPathsResult account_paths_build(const wchar_t* local_application_data,
                                       AccountPaths* paths) noexcept {
    return account_paths_build_slot(local_application_data, 0, paths);
}

AccountPathsResult account_paths_resolve_slot(u32 slot, AccountPaths* paths) noexcept {
    if (!paths) {
        return failure(AccountPathsError::invalid_input);
    }
    wchar_t local_application_data[kCapacity]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_application_data, kCapacity);
    if (length == 0 || length >= kCapacity) {
        return failure(length == 0 ? AccountPathsError::environment : AccountPathsError::capacity,
                       length == 0 ? thread_last_error() : ERROR_INSUFFICIENT_BUFFER);
    }
    return account_paths_build_slot(local_application_data, slot, paths);
}

AccountPathsResult account_paths_resolve(AccountPaths* paths) noexcept {
    return account_paths_resolve_slot(g_selected_slot, paths);
}

AccountPathsResult account_paths_ensure_directories(const AccountPaths& paths) noexcept {
    if (paths.application_directory[0] == 0 || paths.account_directory[0] == 0) {
        return failure(AccountPathsError::invalid_input);
    }
    u32 native_error = 0;
    if (!directory_ensure(paths.application_directory, &native_error)) {
        return failure(AccountPathsError::directory, native_error);
    }
    // A slot directory sits below accounts\; create that root first.
    const u32 application_length = wide_length(paths.application_directory, kCapacity);
    u32 parent_length = wide_length(paths.account_directory, kCapacity);
    while (parent_length && paths.account_directory[parent_length - 1] != L'\\')
        --parent_length;
    if (parent_length > application_length + 1) {
        wchar_t parent[kCapacity];
        copy_bytes(parent, paths.account_directory, (parent_length - 1) * static_cast<u32>(sizeof(wchar_t)));
        parent[parent_length - 1] = 0;
        if (!directory_ensure(parent, &native_error))
            return failure(AccountPathsError::directory, native_error);
    }
    if (!directory_ensure(paths.account_directory, &native_error)) {
        return failure(AccountPathsError::directory, native_error);
    }
    return AccountPathsResult{};
}

} // namespace maicord
