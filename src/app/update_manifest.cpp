#include "maicord/app/update_manifest.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/decimal.hpp"

namespace maicord {
namespace {
enum Field : u32 { format, product, arch, channel, version, issued, expires, bytes, digest, artifact, field_count };
struct Parse {
    UpdateManifest candidate;
    u32 seen;
    UpdateManifestError error;
    bool legacy; // installed protocol-1 generations only
};
bool string(JsonValue value, Utf8View* output) noexcept {
    JsonString parsed{};
    if (!json_value_as_string(value, &parsed) || parsed.has_escapes) return false;
    *output = parsed.encoded;
    return true;
}
i32 hex(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}
bool visit(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& parsed = *static_cast<Parse*>(opaque);
    static constexpr Utf8View names[]{utf8_literal("format"), utf8_literal("product"), utf8_literal("arch"),
        utf8_literal("channel"), utf8_literal("version"), utf8_literal("issued"), utf8_literal("expires"),
        utf8_literal("bytes"), utf8_literal("sha256"), utf8_literal("artifact")};
    u32 field = field_count;
    for (u32 i = 0; i < field_count; ++i)
        if (json_string_equals_ascii(name, names[i])) { field = i; break; }
    if (field == field_count) { parsed.error = UpdateManifestError::field; return false; }
    const u32 bit = 1u << field;
    if (parsed.seen & bit) { parsed.error = UpdateManifestError::duplicate_field; return false; }
    parsed.seen |= bit;
    Utf8View text{};
    u64 number = 0;
    bool valid = true;
    switch (field) {
    case format:
        valid = json_value_to_u64(value, &number) && (number == kUpdateProtocol || (parsed.legacy && number == kUpdateLegacyProtocol));
        parsed.candidate.protocol = static_cast<u8>(number);
        break;
    case product: valid = string(value, &text) && utf8_equal(text, utf8_literal("MaiCord")); break;
    case arch: valid = string(value, &text) && utf8_equal(text, utf8_literal("x64")); break;
    case channel:
        valid = string(value, &text);
        if (valid && utf8_equal(text, utf8_literal("stable"))) parsed.candidate.channel = UpdateChannel::stable;
        else if (valid && utf8_equal(text, utf8_literal("preview"))) parsed.candidate.channel = UpdateChannel::preview;
        else valid = false;
        break;
    case version: valid = json_value_to_u64(value, &parsed.candidate.version); break;
    case issued: valid = json_value_to_u64(value, &parsed.candidate.issued_unix_seconds); break;
    case expires: valid = json_value_to_u64(value, &parsed.candidate.expires_unix_seconds); break;
    case bytes: valid = json_value_to_u64(value, &parsed.candidate.artifact_bytes); break;
    case digest:
        valid = string(value, &text) && text.bytes == 64;
        for (u32 i = 0; valid && i < 32; ++i) {
            const i32 high = hex(text.data[i * 2]), low = hex(text.data[i * 2 + 1]);
            valid = high >= 0 && low >= 0;
            if (valid) parsed.candidate.artifact_sha256[i] = static_cast<u8>((high << 4) | low);
        }
        break;
    case artifact:
        valid = string(value, &text) && text.bytes && text.bytes < kUpdateArtifactNameCapacity;
        if (valid) {
            copy_bytes(parsed.candidate.artifact_name, text.data, text.bytes);
            parsed.candidate.artifact_name_bytes = text.bytes;
        }
        break;
    default: valid = false; break;
    }
    if (!valid) parsed.error = UpdateManifestError::field;
    return valid;
}
bool artifact_matches_version(const UpdateManifest& manifest) noexcept {
    char name[kUpdateArtifactNameCapacity]{};
    const Utf8View prefix = manifest.protocol == kUpdateProtocol ? kUpdateArtifactPrefix : kUpdateLegacyArtifactPrefix;
    copy_bytes(name, prefix.data, prefix.bytes);
    u32 used = prefix.bytes + decimal_write(name + prefix.bytes, manifest.version);
    constexpr Utf8View suffix = utf8_literal(".zip");
    copy_bytes(name + used, suffix.data, suffix.bytes);
    used += suffix.bytes;
    return utf8_equal({name, used}, {manifest.artifact_name, manifest.artifact_name_bytes});
}
UpdateManifestResult verify_manifest(Utf8View document, ConstSlice<u8> signature,
    const P256Key& trusted_key, KdfSha256Workspace& workspace,
    const UpdateManifestPolicy* admission, u64 maximum_artifact_bytes,
    u64 maximum_validity_seconds, Slice<u32> structural_scratch,
    UpdateManifest* output, bool legacy = false) noexcept {
    if (!output || !document.bytes || document.bytes > kUpdateManifestMaximumBytes || signature.count != 64 ||
        !trusted_key.handle || trusted_key.kind != P256Kind::ecdsa || !workspace.storage.data ||
        !structural_scratch.data || !structural_scratch.count ||
        !maximum_artifact_bytes || !maximum_validity_seconds ||
        (admission && (!admission->now_unix_seconds || admission->channel > UpdateChannel::preview)))
        return {UpdateManifestError::invalid_input};
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(document.data), document.bytes}, signature,
        {reinterpret_cast<const u8*>(structural_scratch.data), static_cast<u32>(minimum<usize>(~u32{0},
            static_cast<usize>(structural_scratch.count) * sizeof(u32)))},
        {reinterpret_cast<const u8*>(output), sizeof(*output)},
        {reinterpret_cast<const u8*>(&trusted_key), sizeof(trusted_key)},
        {reinterpret_cast<const u8*>(&workspace), sizeof(workspace)},
        {workspace.storage.data, workspace.storage.capacity},
        {reinterpret_cast<const u8*>(admission), admission ? static_cast<u32>(sizeof(*admission)) : 0u}};
    if (structural_scratch.count > ~u32{0} / sizeof(u32)) return {UpdateManifestError::invalid_input};
    if (!byte_ranges_all_disjoint({ranges, sizeof(ranges) / sizeof(ranges[0])})) return {UpdateManifestError::invalid_input};
    const ConstSlice<u8> parts[]{
        {reinterpret_cast<const u8*>(kUpdateManifestSignatureDomain.data), kUpdateManifestSignatureDomain.bytes},
        {reinterpret_cast<const u8*>(document.data), document.bytes}};
    u8 hashed[32]{};
    if (!sha256(workspace, {parts, 2}, {hashed, sizeof(hashed)}).ok() ||
        !p256_verify_digest(trusted_key, {hashed, sizeof(hashed)}, signature).ok())
        return {UpdateManifestError::signature};
    const auto scan = json_scan_structurals_avx1(document, structural_scratch);
    if (!scan.ok()) return {UpdateManifestError::json};
    Parse parsed{}; parsed.legacy = legacy;
    const auto read = json_visit_root_object(document, {structural_scratch.data, scan.count}, 2, visit, &parsed);
    if (!read.ok()) return {parsed.error == UpdateManifestError::none ? UpdateManifestError::json : parsed.error};
    if (parsed.seen != (1u << field_count) - 1u) return {UpdateManifestError::missing_field};
    const auto& candidate = parsed.candidate;
    if (admission && candidate.channel != admission->channel) return {UpdateManifestError::unsupported};
    if (!candidate.version) return {UpdateManifestError::rollback};
    if (admission) {
        if (candidate.version <= admission->installed_version ||
            candidate.version < admission->highest_accepted_version) return {UpdateManifestError::rollback};
        if (candidate.version == admission->highest_accepted_version &&
            (!admission->has_accepted_manifest_digest ||
                !bytes_equal(hashed, admission->accepted_manifest_sha256, 32)))
            return {UpdateManifestError::rollback};
    }
    if (!candidate.issued_unix_seconds ||
        candidate.expires_unix_seconds <= candidate.issued_unix_seconds ||
        candidate.expires_unix_seconds - candidate.issued_unix_seconds > maximum_validity_seconds ||
        (admission && (candidate.issued_unix_seconds > admission->now_unix_seconds ||
            candidate.expires_unix_seconds <= admission->now_unix_seconds)))
        return {UpdateManifestError::time};
    if (!candidate.artifact_bytes || candidate.artifact_bytes > maximum_artifact_bytes)
        return {UpdateManifestError::artifact_size};
    if (!artifact_matches_version(candidate)) return {UpdateManifestError::artifact_name};
    copy_bytes(parsed.candidate.manifest_sha256, hashed, sizeof(hashed));
    *output = candidate;
    return {UpdateManifestError::none};
}
} // namespace

UpdateManifestResult update_manifest_verify(Utf8View document, ConstSlice<u8> signature,
    const P256Key& trusted_key, KdfSha256Workspace& workspace,
    const UpdateManifestPolicy& policy, Slice<u32> structural_scratch,
    UpdateManifest* output) noexcept {
    return verify_manifest(document, signature, trusted_key, workspace, &policy,
        policy.maximum_artifact_bytes, policy.maximum_validity_seconds, structural_scratch, output);
}

UpdateManifestResult update_manifest_verify_installed(Utf8View document, ConstSlice<u8> signature,
    const P256Key& trusted_key, KdfSha256Workspace& workspace,
    u64 maximum_artifact_bytes, u64 maximum_validity_seconds,
    Slice<u32> structural_scratch, UpdateManifest* output) noexcept {
    return verify_manifest(document, signature, trusted_key, workspace, nullptr,
        maximum_artifact_bytes, maximum_validity_seconds, structural_scratch, output);
}

UpdateManifestResult update_manifest_verify_generation(Utf8View document, ConstSlice<u8> signature,
    const P256Key& trusted_key, KdfSha256Workspace& workspace,
    u64 maximum_artifact_bytes, u64 maximum_validity_seconds,
    Slice<u32> structural_scratch, UpdateManifest* output) noexcept {
    return verify_manifest(document, signature, trusted_key, workspace, nullptr,
        maximum_artifact_bytes, maximum_validity_seconds, structural_scratch, output, true);
}
} // namespace maicord
