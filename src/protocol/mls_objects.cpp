#include "maicord/protocol/mls_objects.hpp"
#include "maicord/protocol/mls_reader.hpp"

namespace maicord {
namespace {
void propagate(MlsReader& outer, const MlsReader& inner, u32 base) noexcept {
    if (inner.error != MlsWireError::none)
        outer.fail(inner.error, base + inner.error_offset);
}
ConstSlice<u8> extensions(MlsReader& reader) noexcept {
    const auto data = reader.vector();
    if (reader.error != MlsWireError::none)
        return {};
    MlsReader list{data};
    while (list.error == MlsWireError::none && list.used < data.count) {
        list.integer(2);
        list.vector();
    }
    propagate(reader, list, reader.used - data.count);
    return data;
}
void leaf(MlsReader& reader, MlsLeafView& view) noexcept {
    const u32 start = reader.used;
    view.encryption_key = reader.vector();
    view.signature_key = reader.vector();
    const u32 credential_offset = reader.used;
    view.credential.type = static_cast<u16>(reader.integer(2));
    if (view.credential.type == 1)
        view.credential.identity = reader.vector();
    else if (view.credential.type == 2) {
        const auto certificates = reader.vector();
        if (reader.error == MlsWireError::none) {
            MlsReader list{certificates};
            while (list.error == MlsWireError::none && list.used < certificates.count)
                list.vector();
            propagate(reader, list, reader.used - certificates.count);
        }
        view.credential.certificates = certificates;
    } else
        reader.fail(MlsWireError::unsupported_credential, credential_offset);
    view.capabilities.versions = reader.integers16();
    view.capabilities.cipher_suites = reader.integers16();
    view.capabilities.extensions = reader.integers16();
    view.capabilities.proposals = reader.integers16();
    view.capabilities.credentials = reader.integers16();
    const u32 source_offset = reader.used;
    view.source = static_cast<MlsLeafSource>(reader.integer(1));
    switch (view.source) {
    case MlsLeafSource::key_package:
        view.not_before = reader.integer(8);
        view.not_after = reader.integer(8);
        break;
    case MlsLeafSource::update:
        break;
    case MlsLeafSource::commit:
        view.parent_hash = reader.vector();
        break;
    default:
        reader.fail(MlsWireError::invalid_leaf_source, source_offset);
        break;
    }
    view.extensions = extensions(reader);
    const u32 tbs_end = reader.used;
    view.signature = reader.vector();
    if (reader.error == MlsWireError::none) {
        view.tbs = {reader.input.data + start, tbs_end - start};
        view.serialized = {reader.input.data + start, reader.used - start};
    }
}
void package(MlsReader& reader, MlsKeyPackageView& view) noexcept {
    const u32 start = reader.used;
    view.version = static_cast<u16>(reader.integer(2));
    view.cipher_suite = static_cast<u16>(reader.integer(2));
    view.init_key = reader.vector();
    leaf(reader, view.leaf);
    view.extensions = extensions(reader);
    const u32 tbs_end = reader.used;
    view.signature = reader.vector();
    if (reader.error == MlsWireError::none) {
        view.tbs = {reader.input.data + start, tbs_end - start};
        view.serialized = {reader.input.data + start, reader.used - start};
    }
}
void group(MlsReader& reader, MlsGroupContextView& view) noexcept {
    const u32 start = reader.used;
    view.version = static_cast<u16>(reader.integer(2));
    view.cipher_suite = static_cast<u16>(reader.integer(2));
    view.group_id = reader.vector();
    view.epoch = reader.integer(8);
    view.tree_hash = reader.vector();
    view.confirmed_transcript_hash = reader.vector();
    view.extensions = extensions(reader);
    if (reader.error == MlsWireError::none)
        view.serialized = {reader.input.data + start, reader.used - start};
}
void encrypted_secrets(MlsReader& reader, MlsEncryptedGroupSecretsView& view) noexcept {
    const u32 start = reader.used;
    view.new_member = reader.vector();
    view.kem_output = reader.vector();
    view.ciphertext = reader.vector();
    if (reader.error == MlsWireError::none)
        view.serialized = {reader.input.data + start, reader.used - start};
}
void welcome(MlsReader& reader, MlsWelcomeView& view) noexcept {
    const u32 start = reader.used;
    view.cipher_suite = static_cast<u16>(reader.integer(2));
    view.secrets = reader.vector();
    if (reader.error == MlsWireError::none) {
        MlsReader entries{view.secrets};
        while (entries.error == MlsWireError::none && entries.used < view.secrets.count) {
            MlsEncryptedGroupSecretsView entry{};
            encrypted_secrets(entries, entry);
        }
        propagate(reader, entries, reader.used - view.secrets.count);
    }
    view.encrypted_group_info = reader.vector();
    if (reader.error == MlsWireError::none)
        view.serialized = {reader.input.data + start, reader.used - start};
}
void group_info(MlsReader& reader, MlsGroupInfoView& view) noexcept {
    const u32 start = reader.used;
    group(reader, view.context);
    view.extensions = extensions(reader);
    view.confirmation_tag = reader.vector();
    view.signer = static_cast<u32>(reader.integer(4));
    const u32 tbs_end = reader.used;
    view.signature = reader.vector();
    if (reader.error == MlsWireError::none) {
        view.serialized = {reader.input.data + start, reader.used - start};
        view.tbs = {reader.input.data + start, tbs_end - start};
    }
}
void message(MlsReader& reader, MlsMessageView& view) noexcept {
    view.version = static_cast<u16>(reader.integer(2));
    if (reader.error != MlsWireError::none)
        return;
    if (view.version != 1) {
        reader.fail(MlsWireError::unsupported_version, 0);
        return;
    }
    view.format = static_cast<MlsWireFormat>(reader.integer(2));
    if (reader.error != MlsWireError::none)
        return;
    switch (view.format) {
    case MlsWireFormat::welcome:
        view.welcome = {};
        welcome(reader, view.welcome);
        break;
    case MlsWireFormat::group_info:
        view.group_info = {};
        group_info(reader, view.group_info);
        if (reader.error == MlsWireError::none && view.group_info.context.version != view.version)
            reader.fail(MlsWireError::version_mismatch, 4);
        break;
    case MlsWireFormat::key_package:
        view.key_package = {};
        package(reader, view.key_package);
        if (reader.error == MlsWireError::none && view.key_package.version != view.version)
            reader.fail(MlsWireError::version_mismatch, 4);
        break;
    default:
        reader.fail(MlsWireError::unsupported_wire_format, 2);
        break;
    }
    if (reader.error == MlsWireError::none)
        view.serialized = {reader.input.data, reader.used};
}
void psk_id(MlsReader& reader, MlsPreSharedKeyIdView& view) noexcept {
    const u32 start = reader.used;
    view.type = static_cast<u8>(reader.integer(1));
    if (view.type == 1)
        view.id = reader.vector();
    else if (view.type == 2) {
        const u32 usage_offset = reader.used;
        view.usage = static_cast<u8>(reader.integer(1));
        if (view.usage < 1 || view.usage > 3)
            reader.fail(MlsWireError::invalid_psk_usage, usage_offset);
        view.group_id = reader.vector();
        view.epoch = reader.integer(8);
    } else
        reader.fail(MlsWireError::invalid_psk_type, start);
    view.nonce = reader.vector();
    if (reader.error == MlsWireError::none)
        view.serialized = {reader.input.data + start, reader.used - start};
}
void group_secrets(MlsReader& reader, MlsGroupSecretsView& view) noexcept {
    const u32 start = reader.used;
    view.joiner_secret = reader.vector();
    const u32 optional_offset = reader.used;
    const u8 present = static_cast<u8>(reader.integer(1));
    if (present > 1)
        reader.fail(MlsWireError::invalid_optional, optional_offset);
    view.has_path_secret = present == 1;
    if (view.has_path_secret)
        view.path_secret = reader.vector();
    view.psks = reader.vector();
    if (reader.error == MlsWireError::none) {
        MlsReader list{view.psks};
        while (list.error == MlsWireError::none && list.used < view.psks.count) {
            MlsPreSharedKeyIdView id{};
            psk_id(list, id);
        }
        propagate(reader, list, reader.used - view.psks.count);
    }
    if (reader.error == MlsWireError::none)
        view.serialized = {reader.input.data + start, reader.used - start};
}
template <typename T, void (*Decode)(MlsReader&, T&) noexcept>
MlsDecodeResult decode(ConstSlice<u8> input, T* output, bool exact) noexcept {
    if (!output || !byte_range_valid(input) ||
        byte_ranges_overlap(input, {reinterpret_cast<const u8*>(output), sizeof(T)}))
        return {0, 0, MlsWireError::invalid_input};
    T candidate{};
    MlsReader reader{input};
    Decode(reader, candidate);
    if (reader.error == MlsWireError::none && exact && reader.used != input.count)
        reader.fail(MlsWireError::trailing_data, reader.used);
    if (reader.error == MlsWireError::none)
        *output = candidate;
    return reader.result();
}
} // namespace
MlsDecodeResult mls_leaf_decode_prefix(ConstSlice<u8> input, MlsLeafView* output) noexcept {
    return decode<MlsLeafView, leaf>(input, output, false);
}
MlsDecodeResult mls_key_package_decode_prefix(ConstSlice<u8> input, MlsKeyPackageView* output) noexcept {
    return decode<MlsKeyPackageView, package>(input, output, false);
}
MlsDecodeResult mls_group_context_decode_prefix(ConstSlice<u8> input,
                                                MlsGroupContextView* output) noexcept {
    return decode<MlsGroupContextView, group>(input, output, false);
}
MlsDecodeResult mls_leaf_decode(ConstSlice<u8> input, MlsLeafView* output) noexcept {
    return decode<MlsLeafView, leaf>(input, output, true);
}
MlsDecodeResult mls_key_package_decode(ConstSlice<u8> input, MlsKeyPackageView* output) noexcept {
    return decode<MlsKeyPackageView, package>(input, output, true);
}
MlsDecodeResult mls_group_context_decode(ConstSlice<u8> input, MlsGroupContextView* output) noexcept {
    return decode<MlsGroupContextView, group>(input, output, true);
}
MlsDecodeResult mls_encrypted_group_secrets_decode_prefix(ConstSlice<u8> input,
                                                          MlsEncryptedGroupSecretsView* output) noexcept {
    return decode<MlsEncryptedGroupSecretsView, encrypted_secrets>(input, output, false);
}
MlsDecodeResult mls_encrypted_group_secrets_decode(ConstSlice<u8> input,
                                                   MlsEncryptedGroupSecretsView* output) noexcept {
    return decode<MlsEncryptedGroupSecretsView, encrypted_secrets>(input, output, true);
}
MlsDecodeResult mls_welcome_decode_prefix(ConstSlice<u8> input, MlsWelcomeView* output) noexcept {
    return decode<MlsWelcomeView, welcome>(input, output, false);
}
MlsDecodeResult mls_welcome_decode(ConstSlice<u8> input, MlsWelcomeView* output) noexcept {
    return decode<MlsWelcomeView, welcome>(input, output, true);
}
MlsDecodeResult mls_group_info_decode_prefix(ConstSlice<u8> input, MlsGroupInfoView* output) noexcept {
    return decode<MlsGroupInfoView, group_info>(input, output, false);
}
MlsDecodeResult mls_group_info_decode(ConstSlice<u8> input, MlsGroupInfoView* output) noexcept {
    return decode<MlsGroupInfoView, group_info>(input, output, true);
}
MlsDecodeResult mls_message_decode_prefix(ConstSlice<u8> input, MlsMessageView* output) noexcept {
    return decode<MlsMessageView, message>(input, output, false);
}
MlsDecodeResult mls_message_decode(ConstSlice<u8> input, MlsMessageView* output) noexcept {
    return decode<MlsMessageView, message>(input, output, true);
}
MlsDecodeResult mls_psk_id_decode_prefix(ConstSlice<u8> input, MlsPreSharedKeyIdView* output) noexcept {
    return decode<MlsPreSharedKeyIdView, psk_id>(input, output, false);
}
MlsDecodeResult mls_psk_id_decode(ConstSlice<u8> input, MlsPreSharedKeyIdView* output) noexcept {
    return decode<MlsPreSharedKeyIdView, psk_id>(input, output, true);
}
MlsDecodeResult mls_group_secrets_decode_prefix(ConstSlice<u8> input,
                                                MlsGroupSecretsView* output) noexcept {
    return decode<MlsGroupSecretsView, group_secrets>(input, output, false);
}
MlsDecodeResult mls_group_secrets_decode(ConstSlice<u8> input, MlsGroupSecretsView* output) noexcept {
    return decode<MlsGroupSecretsView, group_secrets>(input, output, true);
}
} // namespace maicord
