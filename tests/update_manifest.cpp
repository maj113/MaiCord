#include "maicord/app/update_manifest.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
void test_update_manifest(void (*check)(bool, const char*) noexcept) noexcept {
    KdfSha256Workspace workspace{};
    P256Key signer{}, trusted{};
    struct Release {
        KdfSha256Workspace& hash; P256Key& signer; P256Key& trusted;
        ~Release() noexcept { p256_release(&trusted); p256_release(&signer); kdf_sha256_release(&hash); }
    } release{workspace, signer, trusted};
    u8 scalar[32]{}; scalar[31] = 1;
    u8 point[65]{};
    const bool initialized = kdf_sha256_initialize(&workspace).ok() &&
        p256_import_private(&signer, P256Kind::ecdsa, {scalar, 32}).ok() &&
        p256_export_public(signer, {point, 65}).ok() &&
        p256_import_public(&trusted, P256Kind::ecdsa, {point, 65}).ok();
    check(initialized, "update: test-only P-256 trust and reusable SHA-256 workspace initialize");
    if (!initialized) return;
    u32 scratch[256]{};
    u8 signature[64]{};
    UpdateManifestPolicy policy{4, 5, 100, 64 * 1024 * 1024, 1000, UpdateChannel::stable};
    UpdateManifest result{};
    auto sign = [&](Utf8View document, bool domain = true) noexcept {
        const ConstSlice<u8> parts[]{
            {reinterpret_cast<const u8*>(kUpdateManifestSignatureDomain.data), kUpdateManifestSignatureDomain.bytes},
            {reinterpret_cast<const u8*>(document.data), document.bytes}};
        u8 digest[32]{};
        return sha256(workspace, {domain ? parts : parts + 1, domain ? 2u : 1u}, {digest, 32}).ok() &&
            p256_sign_digest(signer, {digest, 32}, {signature, 64}).ok();
    };
    auto verify = [&](Utf8View document) noexcept {
        return update_manifest_verify(document, {signature, 64}, trusted, workspace, policy,
            {scratch, 256}, &result);
    };
    const auto good = utf8_literal(R"({"format":2,"product":"MaiCord","arch":"x64","channel":"stable","version":6,"issued":90,"expires":200,"bytes":4096,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","artifact":"MaiCord-x64-p2-6.zip"})");
    check(sign(good) && verify(good).ok() && result.version == 6 && result.artifact_bytes == 4096 &&
        result.artifact_sha256[0] == 1 && result.artifact_sha256[31] == 0xef &&
        utf8_equal({result.artifact_name, result.artifact_name_bytes}, utf8_literal("MaiCord-x64-p2-6.zip")),
        "update: trusted detached signature admits owned bounded manifest snapshot");
    const UpdateManifest accepted = result;
    auto reject = [&](Utf8View document, UpdateManifestError expected) noexcept {
        const bool signed_value = sign(document);
        const auto response = verify(document);
        check(signed_value && response.error == expected && bytes_equal(&accepted, &result, sizeof(result)),
            "update: signed invalid metadata fails atomically");
    };
    signature[7] ^= 1;
    check(verify(good).error == UpdateManifestError::signature && bytes_equal(&accepted, &result, sizeof(result)),
        "update: signature modification cannot mutate prior offer");
    check(sign(good, false) && verify(good).error == UpdateManifestError::signature,
        "update: signatures without the update domain are rejected");
    const Utf8View bad_fields[]{
        utf8_literal(R"({"format":2,"format":2})"),
        utf8_literal(R"({"format":2,"for\u006dat":1})"),
        utf8_literal(R"({"format":1})"), utf8_literal(R"({"format":3})"), utf8_literal(R"({"format":2,"unknown":0})"),
        utf8_literal(R"({"format":2,"sha256":"gg"})"),
        utf8_literal(R"({"format":2,"version":18446744073709551616})"),
        utf8_literal(R"({"format":2,"arch":"arm64"})"),
        utf8_literal(R"({"format":2,"product":"M\u0061iCord"})")};
    for (u32 i = 0; i < sizeof(bad_fields) / sizeof(bad_fields[0]); ++i)
        reject(bad_fields[i], i < 2 ? UpdateManifestError::duplicate_field : UpdateManifestError::field);
    reject(utf8_literal(R"({"format":2})"), UpdateManifestError::missing_field);
    reject(utf8_literal(R"({"format":2,})"), UpdateManifestError::json);
    // Exercise policy separately from signature/JSON rejection.
    policy.installed_version = 6;
    reject(good, UpdateManifestError::rollback);
    policy.installed_version = 4; policy.highest_accepted_version = 7;
    reject(good, UpdateManifestError::rollback);
    policy.highest_accepted_version = 6;
    reject(good, UpdateManifestError::rollback);
    policy.has_accepted_manifest_digest = true;
    copy_bytes(policy.accepted_manifest_sha256, accepted.manifest_sha256, 32);
    check(sign(good) && verify(good).ok(), "update: same accepted version permits interrupted download retry");
    char changed[kUpdateManifestMaximumBytes]{};
    copy_bytes(changed, good.data, good.bytes);
    changed[good.bytes] = ' ';
    reject({changed, good.bytes + 1}, UpdateManifestError::rollback);
    policy.channel = UpdateChannel::preview;
    reject(good, UpdateManifestError::unsupported);
    policy.channel = UpdateChannel::stable; policy.now_unix_seconds = 200;
    reject(good, UpdateManifestError::time);
    policy.now_unix_seconds = 89;
    reject(good, UpdateManifestError::time);
    policy.now_unix_seconds = 100; policy.maximum_validity_seconds = 100;
    reject(good, UpdateManifestError::time);
    policy.maximum_validity_seconds = 1000; policy.maximum_artifact_bytes = 4095;
    reject(good, UpdateManifestError::artifact_size);
    policy.maximum_artifact_bytes = 64 * 1024 * 1024;
    const auto traversal = utf8_literal(R"({"format":2,"product":"MaiCord","arch":"x64","channel":"stable","version":6,"issued":90,"expires":200,"bytes":4096,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","artifact":"../MaiCord-x64-p2-6.zip"})");
    policy.highest_accepted_version = 5;
    reject(traversal, UpdateManifestError::artifact_name);
    const auto previous_protocol = utf8_literal(R"({"format":2,"product":"MaiCord","arch":"x64","channel":"stable","version":6,"issued":90,"expires":200,"bytes":4096,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","artifact":"MaiCord-x64-6.zip"})");
    reject(previous_protocol, UpdateManifestError::artifact_name);
    policy.highest_accepted_version = 6;
    check(sign(good), "update: valid signature restored for capacity checks");
    check(update_manifest_verify(good, {signature, 64}, trusted, workspace, policy, {scratch, 1}, &result).error ==
        UpdateManifestError::json && bytes_equal(&accepted, &result, sizeof(result)),
        "update: bounded parser exhaustion preserves prior offer");
    check(update_manifest_verify(good, {signature, 63}, trusted, workspace, policy, {scratch, 256}, &result).error ==
        UpdateManifestError::invalid_input,
        "update: exact P1363 signature length required");
    check(update_manifest_verify(good, {signature, 64}, trusted, workspace, policy,
        {reinterpret_cast<u32*>(&result), sizeof(result) / sizeof(u32)}, &result).error == UpdateManifestError::invalid_input &&
        bytes_equal(&accepted, &result, sizeof(result)), "update: output/scratch alias rejected before mutation");

    auto installed = [&](Utf8View document, u64 maximum_bytes = 64 * 1024 * 1024,
        u64 maximum_validity = 1000) noexcept {
        return update_manifest_verify_installed(document, {signature, 64}, trusted, workspace,
            maximum_bytes, maximum_validity, {scratch, 256}, &result);
    };
    policy.installed_version = 20; policy.highest_accepted_version = 30; policy.now_unix_seconds = 500;
    check(sign(good) && installed(good).ok() && bytes_equal(&accepted, &result, sizeof(result)) &&
        verify(good).error == UpdateManifestError::rollback,
        "update: retained installed provenance survives expiry and floors without admitting an old offer");
    check(installed(good, 4095).error == UpdateManifestError::artifact_size &&
        installed(good, 64 * 1024 * 1024, 100).error == UpdateManifestError::time &&
        bytes_equal(&accepted, &result, sizeof(result)),
        "update: installed authentication still enforces trusted package and validity bounds");
    const auto invalid_interval = utf8_literal(R"({"format":2,"product":"MaiCord","arch":"x64","channel":"stable","version":6,"issued":200,"expires":200,"bytes":4096,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","artifact":"MaiCord-x64-p2-6.zip"})");
    check(sign(invalid_interval) && installed(invalid_interval).error == UpdateManifestError::time &&
        bytes_equal(&accepted, &result, sizeof(result)),
        "update: installed authentication rejects a signed invalid interval atomically");
    check(sign(traversal) && installed(traversal).error == UpdateManifestError::artifact_name &&
        bytes_equal(&accepted, &result, sizeof(result)),
        "update: installed authentication retains exact signed artifact naming");
    check(sign(good), "update: installed signature restored");
    signature[0] ^= 1;
    check(installed(good).error == UpdateManifestError::signature &&
        bytes_equal(&accepted, &result, sizeof(result)),
        "update: installed authentication never substitutes checksums for publisher signatures");
    check(sign(good) && update_manifest_verify_installed(good, {signature, 64}, trusted, workspace,
        64 * 1024 * 1024, 1000, {reinterpret_cast<u32*>(&result), sizeof(result) / sizeof(u32)}, &result).error ==
            UpdateManifestError::invalid_input && bytes_equal(&accepted, &result, sizeof(result)),
        "update: installed authentication rejects output/scratch alias before mutation");
}
} // namespace maicord
