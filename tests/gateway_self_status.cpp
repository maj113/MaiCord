#include "maicord/protocol/gateway_presence.hpp"
#include "maicord/protocol/self_status_settings.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/core/memory.hpp"
#include "gateway_self_status_owner.hpp"

namespace maicord {
void test_gateway_self_status(void (*check)(bool, const char*) noexcept) noexcept {
    u8 wire[512]{};
    auto result = gateway_write_presence_update({0, GatewayUserStatus::invisible, false, false}, nullptr, nullptr,
                                                {wire, sizeof(wire)});
    check(result.ok() && utf8_equal({reinterpret_cast<const char*>(wire), result.bytes},
        utf8_literal("{\"op\":3,\"d\":{\"since\":null,\"activities\":[],\"status\":\"invisible\",\"afk\":false}}")),
        "self status: opcode3 Invisible is explicit desired status rather than Offline presence");
    wire[0] = 77;
    check(gateway_write_presence_update({0, static_cast<GatewayUserStatus>(4), false, false}, nullptr, nullptr,
                                        {wire, sizeof(wire)}).error == GatewayCommandError::invalid_input &&
              wire[0] == 77 &&
          gateway_write_presence_update({42, GatewayUserStatus::online, true, false}, nullptr, nullptr,
                                        {wire, sizeof(wire)}).error == GatewayCommandError::invalid_input,
          "self status: unsupported status and nonIdle since are rejected before output");
    check(gateway_write_presence_update({~u64{0}, GatewayUserStatus::idle, true, true}, nullptr, nullptr, {wire, 1})
                  .error == GatewayCommandError::output_too_small && wire[0] == 77,
          "self status: presence writer checks complete frame capacity first");

    constexpr u8 original_group[] = {
        10,10,10,6,'o','n','l','i','n','e',72,1, // status with unknown wrapper field9
        18,3,10,1,'X',                         // opaque custom status
        26,2,8,1,                             // game visibility
        33,1,0,0,0,0,0,0,0,                   // expiry
        42,4,8,123,72,2,                      // creation with unknown wrapper field9
        232,1,3};                             // unknown group field29
    u8 proto[128]{8,1,90,static_cast<u8>(sizeof(original_group))};
    copy_bytes(proto + 4, original_group, sizeof(original_group));
    u8 group[512]{};
    // Settings arrive as base64; encode raw protobuf fixtures first.
    const auto decode_raw = [](ConstSlice<u8> raw, Slice<u8> output, SelfStatusSettingsProjection* out) noexcept {
        char text[256]{};
        const auto base = base64_encode(raw, {text, sizeof(text)});
        return base.ok() ? self_status_settings_decode_base64({text, base.bytes}, output, out)
                         : SelfStatusSettingsError::input;
    };
    SelfStatusSettingsProjection projection{};
    check(decode_raw({proto, 4 + sizeof(original_group)}, {group, sizeof(group)}, &projection) ==
              SelfStatusSettingsError::none && projection.present && projection.known &&
              projection.status == GatewayUserStatus::online && projection.expires_at_ms == 1 &&
              projection.created_at_ms == 123 && projection.group_bytes == sizeof(original_group) &&
              bytes_equal(group, original_group, sizeof(original_group)),
          "self status: real settings field11 extracts status/expiry/creation and retains opaque siblings");
    u8 scratch[1024]{}, patch[2048]{};
    const auto written = self_status_settings_write_patch({group, projection.group_bytes}, GatewayUserStatus::invisible,
                                                          1'234'567, {scratch, sizeof(scratch)}, {patch, sizeof(patch)});
    u8 changed[512]{};
    SelfStatusSettingsProjection updated{};
    const Utf8View encoded{reinterpret_cast<const char*>(patch + 13), written.bytes >= 15 ? written.bytes - 15 : 0};
    check(written.ok() && self_status_settings_decode_base64(encoded, {changed, sizeof(changed)}, &updated) ==
              SelfStatusSettingsError::none && updated.known && updated.status == GatewayUserStatus::invisible &&
              updated.expires_at_ms == 0 && updated.created_at_ms == 1'234'567,
          "self status: settings PATCH roundtrip changes desired status and createdAt and clears expiry");
    const auto contains = [&](const u8* data, u32 count) noexcept {
        for (u32 i = 0; i + count <= updated.group_bytes; ++i)
            if (bytes_equal(changed + i, data, count)) return true;
        return false;
    };
    const u8 custom[]{18,3,10,1,'X'}, visible[]{26,2,8,1}, opaque[]{232,1,3}, wrapper_unknown[]{72,1};
    check(contains(custom, sizeof(custom)) && contains(visible, sizeof(visible)) && contains(opaque, sizeof(opaque)) &&
              contains(wrapper_unknown, sizeof(wrapper_unknown)),
          "self status: replacement group retains custom status/game visibility/unknown group and wrapper fields");
    const u8 missing[]{8,1}, empty[]{90,0}, invalid[]{90,2,10,5};
    check(decode_raw({missing, sizeof(missing)}, {group, sizeof(group)}, &projection) ==
              SelfStatusSettingsError::none && !projection.present && !projection.known &&
          decode_raw({empty, sizeof(empty)}, {group, sizeof(group)}, &projection) ==
              SelfStatusSettingsError::none && projection.present && !projection.known,
          "self status: missing and empty authoritative group remain unknown rather than fabricated Online");
    group[0] = 88; const auto prior = projection;
    check(decode_raw({invalid, sizeof(invalid)}, {group, sizeof(group)}, &projection) ==
              SelfStatusSettingsError::protobuf && group[0] == 88 && projection.present == prior.present &&
          self_status_settings_decode_base64(utf8_literal("*AAA"), {group, sizeof(group)}, &projection) ==
              SelfStatusSettingsError::base64,
          "self status: malformed protobuf/base64 cannot replace previous projection or opaque group");
    check(decode_raw({proto, 4 + sizeof(original_group)}, {group, 1}, &projection) ==
              SelfStatusSettingsError::capacity && group[0] == 88,
          "self status: bounded extraction reports capacity before copying any partial group");

    GatewaySelfStatusOwner owner{};
    check(gateway_self_status_initialize(&owner, 7, {0, GatewayUserStatus::online, false, false}) &&
              !owner.authoritative && gateway_self_status_bind(&owner, 9) &&
              !gateway_self_status_request(&owner, 7, 9, GatewayUserStatus::invisible),
          "self status: Identify seed never masquerades as saved desired status");
    gateway_self_status_observe(&owner, GatewayUserStatus::idle);
    check(owner.authoritative && owner.confirmed_status == GatewayUserStatus::idle &&
              !gateway_self_status_bind(&owner, 10) &&
              !gateway_self_status_request(&owner, 8, 9, GatewayUserStatus::online) &&
              !gateway_self_status_request(&owner, 7, 10, GatewayUserStatus::online),
          "self status: authenticated self and exact account protect status requests");
    const u64 original_revision = owner.revision;
    check(gateway_self_status_request(&owner, 7, 9, GatewayUserStatus::idle) && owner.revision == original_revision &&
              gateway_self_status_request(&owner, 7, 9, GatewayUserStatus::invisible) &&
              owner.desired.status == GatewayUserStatus::invisible && owner.confirmed_status == GatewayUserStatus::idle,
          "self status: desired and remotely confirmed values are distinct and duplicate selection coalesces");
    gateway_self_status_admit(&owner, owner.revision, 100);
    gateway_self_status_request(&owner, 7, 9, GatewayUserStatus::dnd);
    gateway_self_status_complete(&owner, true);
    check(gateway_self_status_pending(owner) && owner.desired.status == GatewayUserStatus::dnd &&
              owner.transmitted_revision != owner.revision,
          "self status: old inFlight completion cannot consume newer desired choice");
    for (u32 i = 1; i < 5; ++i) gateway_self_status_admit(&owner, owner.revision, 100 + i);
    check(gateway_self_status_deadline(&owner, 20'099) == 20'100,
          "self status: sixth presence frame waits for exact oldest send plus20seconds");
    gateway_self_status_reset_connection(&owner);
    check(gateway_self_status_pending(owner) && gateway_self_status_deadline(&owner, 20'099) == 20'100 &&
              gateway_self_status_deadline(&owner, 20'100) == 0,
          "self status: reconnect reapplies latest desired state without resetting presence rate window");
}
} // namespace maicord
