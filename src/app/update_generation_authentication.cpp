#include "maicord/app/update_generation_authentication.hpp"
#include "maicord/core/byte_range.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
UpdateGenerationResult fail(UpdateGenerationError error, DWORD native = 0) noexcept { return {error, native}; }
bool owned(const UpdateGenerationAuthenticationContext& context) noexcept {
    for (auto file : context.files) if (file) return true;
    return context.reopened.directory || context.package_result.cleanup_hash || context.package_result.cleanup_error;
}
bool file_size(HANDLE file, u64* bytes) noexcept {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) return false;
    *bytes = static_cast<u64>(size.QuadPart); return true;
}
bool read_exact(HANDLE file, void* output, u32 bytes, DWORD* native) noexcept {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) { *native = GetLastError(); return false; }
    u32 done = 0;
    while (done < bytes) {
        DWORD got = 0;
        if (!ReadFile(file, static_cast<u8*>(output) + done, bytes - done, &got, nullptr)) { *native = GetLastError(); return false; }
        if (!got || got > bytes - done) { *native = ERROR_INVALID_DATA; return false; }
        done += got;
    }
    return true;
}
UpdateGenerationResult release(UpdateGenerationAuthenticationContext*, bool retain_members) noexcept;
bool verify(void* opaque, UpdateGenerationTransaction* transaction,
    const UpdatePackageDescriptor& expected, DWORD* native) noexcept {
    auto* context = static_cast<UpdateGenerationAuthenticationContext*>(opaque);
    if (!context || !native) return false;
    *native = ERROR_INVALID_PARAMETER;
    if (!transaction || !context->key || !context->workspace || !context->maximum_artifact_bytes ||
        context->maximum_artifact_bytes > kUpdatePackageMaximumBytes || !context->maximum_validity_seconds || owned(*context)) {
        context->last_result = fail(UpdateGenerationError::invalid_input); return false;
    }
    // Refuse caller aliasing before manifest reads/scratch writes or ownership
    // changes. The optional retained candidate must be independently owned.
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(context), sizeof(*context)},
        {reinterpret_cast<const u8*>(transaction), sizeof(*transaction)},
        {reinterpret_cast<const u8*>(&expected), sizeof(expected)},
        {reinterpret_cast<const u8*>(context->key), sizeof(*context->key)},
        {reinterpret_cast<const u8*>(context->workspace), sizeof(*context->workspace)},
        {context->workspace->storage.data, context->workspace->storage.capacity},
        {reinterpret_cast<const u8*>(context->retained_candidate), context->retained_candidate ? static_cast<u32>(sizeof(*context->retained_candidate)) : 0}};
    // expected may be a const reference into transaction.state.
    for (u32 i = 0; i < sizeof(ranges) / sizeof(ranges[0]); ++i) {
        if (!byte_range_valid(ranges[i])) { context->last_result = fail(UpdateGenerationError::invalid_input); return false; }
        for (u32 j = 0; j < i; ++j) if (!(i == 2 && j == 1) && byte_ranges_overlap(ranges[i], ranges[j])) {
            context->last_result = fail(UpdateGenerationError::invalid_input); return false;
        }
    }
    context->last_result = update_generation_validate(transaction);
    if (!context->last_result.ok()) { *native = context->last_result.native_error; return false; }
    u8 fingerprint[32]{};
    context->last_result = update_generation_inventory_fingerprint(*context->key, *context->workspace, {fingerprint, 32});
    if (!context->last_result.ok() || !bytes_equal(fingerprint, transaction->publisher_inventory_sha256, 32)) {
        context->last_result = fail(UpdateGenerationError::authenticity, ERROR_INVALID_DATA); *native = ERROR_INVALID_DATA; return false;
    }
    UpdateGenerationDirectory* directory = &context->reopened;
    if (context->retained_candidate && context->retained_candidate->transaction == transaction &&
        bytes_equal(context->retained_candidate->document_sha256, expected.document_sha256, 32)) {
        directory = context->retained_candidate;
        context->last_result = update_generation_directory_validate(directory);
    } else {
        context->last_result = update_generation_directory_open(transaction, {expected.document_sha256, 32}, false, directory);
    }
    const u32 files = update_generation_file_count(expected);
    if (context->last_result.ok() && (!expected.member_count || expected.member_count > kUpdatePackageMaximumMembers))
        context->last_result = fail(UpdateGenerationError::state, ERROR_INVALID_DATA);
    if (context->last_result.ok()) {
        for (u32 i = 0; i < files; ++i) {
            context->last_result = update_generation_file_open(directory, update_generation_file_name(expected, i), false, &context->files[i]);
            if (!context->last_result.ok()) break;
        }
    }
    u64 document_bytes = 0, signature_bytes = 0, package_bytes = 0;
    if (context->last_result.ok()) {
        if (!file_size(context->files[kUpdateGenerationOfferSlot], &document_bytes) ||
            !file_size(context->files[kUpdateGenerationSignatureSlot], &signature_bytes) ||
            !file_size(context->files[kUpdateGenerationPackageSlot], &package_bytes))
            context->last_result = fail(UpdateGenerationError::read, GetLastError());
        else if (!document_bytes || document_bytes > kUpdateManifestMaximumBytes || signature_bytes != 64 ||
            !package_bytes || package_bytes > context->maximum_artifact_bytes || package_bytes != expected.package_bytes)
            context->last_result = fail(UpdateGenerationError::size, ERROR_INVALID_DATA);
    }
    DWORD read_error = 0;
    if (context->last_result.ok() &&
        (!read_exact(context->files[kUpdateGenerationOfferSlot], context->document, static_cast<u32>(document_bytes), &read_error) ||
         !read_exact(context->files[kUpdateGenerationSignatureSlot], context->signature, 64, &read_error)))
        context->last_result = fail(UpdateGenerationError::read, read_error);
    if (context->last_result.ok()) {
        UpdateStage stage{}; stage.file = context->files[kUpdateGenerationPackageSlot]; stage.bytes = package_bytes;
        const UpdatePackageOffer offer{{context->document, static_cast<u32>(document_bytes)}, {context->signature, 64}};
        UpdatePackageDescriptor derived{};
        context->package_result = update_package_verify_installed(&stage, offer, *context->key, *context->workspace,
            context->maximum_validity_seconds, {context->structural_scratch, 256},
            {context->files + kUpdateGenerationFirstMemberSlot, expected.member_count}, &derived);
        if (!context->package_result.ok() || !update_package_descriptor_equal(derived, expected))
            context->last_result = fail(UpdateGenerationError::authenticity,
                context->package_result.native_error ? context->package_result.native_error : ERROR_INVALID_DATA);
        else context->last_result = update_generation_directory_validate(directory);
    }
    const auto primary = context->last_result;
    const auto released = release(context, primary.ok() && context->retain_selected_package);
    context->last_result = primary.ok() ? released : primary;
    *native = context->last_result.native_error;
    return context->last_result.ok();
}
} // namespace

UpdateGenerationResult update_generation_inventory_fingerprint(const P256Key& key,
    KdfSha256Workspace& workspace, Slice<u8> output) noexcept {
    if (!key.handle || key.kind != P256Kind::ecdsa || key.has_private || !output.data || output.count < 32 ||
        byte_ranges_overlap({reinterpret_cast<const u8*>(&key), sizeof(key)}, {output.data, output.count}))
        return fail(UpdateGenerationError::invalid_input);
    u8 point[65]{};
    if (!p256_export_public(key, {point, 65}).ok() || point[0] != 4) return fail(UpdateGenerationError::authenticity, ERROR_INVALID_DATA);
    const ConstSlice<u8> parts[]{
        {reinterpret_cast<const u8*>(kUpdatePublisherInventoryDomain.data), kUpdatePublisherInventoryDomain.bytes}, {point, 65}};
    const auto hashed = sha256(workspace, {parts, 2}, output);
    return hashed.ok() ? UpdateGenerationResult{} : fail(UpdateGenerationError::authenticity, ERROR_INVALID_DATA);
}
UpdateGenerationAuthenticator update_generation_authenticator(UpdateGenerationAuthenticationContext* context) noexcept { return {context, verify}; }
namespace {
UpdateGenerationResult release(UpdateGenerationAuthenticationContext* context, bool retain_members) noexcept {
    if (!context) return fail(UpdateGenerationError::invalid_input);
    const auto crypto = update_package_cleanup_release(&context->package_result);
    if (!crypto.ok()) return fail(UpdateGenerationError::release, crypto.native_error ? crypto.native_error : ERROR_INVALID_DATA);
    // Selection keeps the member pins and closes the provenance files.
    for (u32 i = 0; i < kUpdateGenerationMaximumFiles; ++i) if (context->files[i] && !(retain_members && i >= kUpdateGenerationFirstMemberSlot)) {
        auto& file = context->files[i];
        if (!CloseHandle(file)) return fail(UpdateGenerationError::release, GetLastError());
        file = nullptr;
    }
    if (context->reopened.directory) {
        const auto directory = update_generation_directory_release(&context->reopened, false);
        if (!directory.ok()) return directory;
    }
    SecureZeroMemory(&context->reopened, sizeof(context->reopened));
    SecureZeroMemory(context->document, sizeof(context->document));
    SecureZeroMemory(context->signature, sizeof(context->signature));
    SecureZeroMemory(context->structural_scratch, sizeof(context->structural_scratch));
    return {};
}
} // namespace
UpdateGenerationResult update_generation_authentication_release(UpdateGenerationAuthenticationContext* context) noexcept {
    return release(context, false);
}
namespace {
UpdateGenerationResult select_package(UpdateGenerationTransaction* transaction,
    UpdateGenerationAuthenticationContext* context, const UpdateGenerationLaunchAdmission* admission,
    UpdatePackage* output) noexcept {
    if (!transaction || !context || !output || owned(*context) || context->retain_selected_package)
        return fail(UpdateGenerationError::invalid_input);
    for (auto file : output->files) if (file) return fail(UpdateGenerationError::invalid_input);
    if (admission) {
        const ConstSlice<u8> input{reinterpret_cast<const u8*>(admission), sizeof(*admission)};
        const ConstSlice<u8> mutable_ranges[]{
            {reinterpret_cast<const u8*>(transaction), sizeof(*transaction)},
            {reinterpret_cast<const u8*>(context), sizeof(*context)},
            {reinterpret_cast<const u8*>(output), sizeof(*output)},
            {reinterpret_cast<const u8*>(context->key), context->key ? static_cast<u32>(sizeof(*context->key)) : 0},
            {reinterpret_cast<const u8*>(context->workspace), context->workspace ? static_cast<u32>(sizeof(*context->workspace)) : 0},
            {context->workspace ? context->workspace->storage.data : nullptr, context->workspace ? context->workspace->storage.capacity : 0},
            {reinterpret_cast<const u8*>(context->retained_candidate), context->retained_candidate ? static_cast<u32>(sizeof(*context->retained_candidate)) : 0}};
        if (!byte_range_valid(input)) return fail(UpdateGenerationError::invalid_input);
        for (const auto range : mutable_ranges)
            if (!byte_range_valid(range) || byte_ranges_overlap(input, range))
                return fail(UpdateGenerationError::invalid_input);
    }
    const ConstSlice<u8> destination{reinterpret_cast<const u8*>(output), sizeof(*output)};
    if (!byte_range_valid(destination) ||
        (admission && (!byte_range_valid({reinterpret_cast<const u8*>(admission), sizeof(*admission)}) ||
            byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(admission), sizeof(*admission)}) ||
            byte_ranges_overlap({reinterpret_cast<const u8*>(context), sizeof(*context)},
                {reinterpret_cast<const u8*>(admission), sizeof(*admission)}))) ||
        byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(transaction), sizeof(*transaction)}) ||
        byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(context), sizeof(*context)}) ||
        (context->retained_candidate && byte_ranges_overlap(destination,
            {reinterpret_cast<const u8*>(context->retained_candidate), sizeof(*context->retained_candidate)})) ||
        (context->key && byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(context->key), sizeof(*context->key)})) ||
        (context->workspace && (byte_ranges_overlap(destination,
            {reinterpret_cast<const u8*>(context->workspace), sizeof(*context->workspace)}) ||
            byte_ranges_overlap(destination, {context->workspace->storage.data, context->workspace->storage.capacity}))))
        return fail(UpdateGenerationError::invalid_input);
    if (admission) {
        if (!transaction->record_present || transaction->record_owner || transaction->record_temporary ||
            transaction->state.phase != UpdateGenerationPhase::pending)
            return fail(UpdateGenerationError::state);
        if (!admission->sequence || admission->sequence != transaction->state.sequence ||
            !bytes_equal(admission->transaction.bytes, transaction->state.transaction.bytes, 16) ||
            !bytes_equal(admission->document_sha256, transaction->state.active.package.document_sha256, 32))
            return fail(UpdateGenerationError::identity);
        u8 record[kUpdateGenerationRecordBytes]{};
        const auto valid = update_generation_encode(transaction->state, {record, sizeof(record)});
        if (!valid.ok()) return valid;
    }
    context->retain_selected_package = true;
    UpdatePackageDescriptor descriptor{};
    UpdateGenerationResult selected{};
    if (admission) {
        selected = update_generation_validate(transaction);
        DWORD native = 0;
        if (selected.ok() && !verify(context, transaction, transaction->state.active.package, &native))
            selected = fail(UpdateGenerationError::authenticity, native);
        if (selected.ok()) selected = update_generation_validate(transaction);
        if (selected.ok()) descriptor = transaction->state.active.package;
    } else {
        selected = update_generation_select(transaction, update_generation_authenticator(context), &descriptor);
    }
    context->retain_selected_package = false;
    const u32 members = descriptor.member_count <= kUpdatePackageMaximumMembers ? descriptor.member_count : 0;
    bool pinned = selected.ok() && members;
    for (u32 i = 0; pinned && i < members; ++i) pinned = context->files[kUpdateGenerationFirstMemberSlot + i] != nullptr;
    if (!pinned) {
        const auto cleanup = release(context, false);
        if (!selected.ok()) return selected;
        return cleanup.ok() ? fail(UpdateGenerationError::authenticity, ERROR_INVALID_DATA) : cleanup;
    }
    UpdatePackage package{}; package.descriptor = descriptor;
    for (u32 i = 0; i < members; ++i) {
        package.files[i] = context->files[kUpdateGenerationFirstMemberSlot + i]; context->files[kUpdateGenerationFirstMemberSlot + i] = nullptr;
    }
    *output = package; return {};
}
} // namespace
UpdateGenerationResult update_generation_select_package(UpdateGenerationTransaction* transaction,
    UpdateGenerationAuthenticationContext* context, UpdatePackage* output) noexcept {
    return select_package(transaction, context, nullptr, output);
}
UpdateGenerationResult update_generation_select_pending_package_for_launch(UpdateGenerationTransaction* transaction,
    UpdateGenerationAuthenticationContext* context, const UpdateGenerationLaunchAdmission& admission,
    UpdatePackage* output) noexcept {
    return select_package(transaction, context, &admission, output);
}
} // namespace maicord
