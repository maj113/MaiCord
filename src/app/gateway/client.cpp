#include "client_internal.hpp"
#include "maicord/app/net/rest_client.hpp"

namespace maicord {

bool gateway_user_client_configuration_validate(
    const GatewayUserClientConfiguration& configuration) noexcept {
    u8 output[kGatewayUserMaximumCommandBytes];
    const GatewayCommandWriteResult written = gateway_write_user_identify(
        GatewayUserIdentify{utf8_literal("x"), configuration.properties, configuration.since_ms,
                            configuration.status, configuration.has_since, configuration.afk},
        Slice<u8>{output, static_cast<u32>(sizeof(output))});
    SecureZeroMemory(output, sizeof(output));
    return written.ok();
}

namespace {

#if defined(MAICORD_GATEWAY_SELF_STATUS_TEST)
GatewaySelfStatusTestSubmit self_status_test_submit;
#endif

} // namespace

GatewayClientResult gateway_client_start_user(GatewayClient* client, AccountProfile* profile,
                                              const GatewayUserClientConfiguration* configuration,
                                              const GatewayEndpoint* gateway_endpoint,
                                              const GatewayUserSessionSnapshot* session) noexcept {
    if (!profile || (session && (session->profile_id != profile->metadata.profile_id ||
                                 (session->has_resume && !gateway_resume_validate(session->resume)) ||
                                 (!session->has_resume && session->resume.valid)))) {
        return invalid_input_result();
    }
    const AccountProfileMetadata metadata = profile->metadata;
    const GatewayClientResult started = start_client(
        client, &profile->credential, &metadata, configuration, gateway_endpoint,
        session && session->has_resume ? &session->resume : nullptr,
        session && session->has_resume ? session->self_user_id : 0);
    if (!profile->credential.data) {
        reset_value<AccountProfile>(profile);
    }
    return started;
}

GatewayClientResult gateway_client_stop_return_user_profile(GatewayClient* client, u32 timeout_milliseconds,
                                                            AccountProfile* returned_profile) noexcept {
    const auto* initial_runtime =
        client && client->runtime ? static_cast<const GatewayClientRuntime*>(client->runtime) : nullptr;
    if (!client || !client->worker.thread || !initial_runtime || !returned_profile ||
        returned_profile->credential.data || returned_profile->credential.locked ||
        returned_profile->credential.bytes != 0 || returned_profile->credential.capacity != 0 ||
        returned_profile->metadata.profile_id != 0 || !initial_runtime->user) {
        return invalid_input_result();
    }
    if (WaitForSingleObject(client->worker.thread, 0) == WAIT_TIMEOUT) {
        SetEvent(initial_runtime->mailbox_cancel);
        network_worker_request_stop(&client->worker);
    }
    const NetworkWorkerResult joined = network_worker_join(&client->worker, timeout_milliseconds);
    GatewayClientResult value = result(client);
    if (!joined.ok()) {
        value.error = GatewayClientError::worker;
        value.native_error = joined.native_error;
        return value;
    }
    if (client->notification_event) {
        CloseHandle(client->notification_event);
    }
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    CloseHandle(runtime->mailbox_space);
    CloseHandle(runtime->mailbox_cancel);
    voice_receive_stop(&runtime->voice_receive);
    // Decode threads must stop asking a freed receive path for keyframes.
    if (runtime->video_frames.bind_keyframes)
        runtime->video_frames.bind_keyframes(runtime->video_frames.context, {});
    gateway_voice_release(&runtime->voice);
    gateway_stream_session_release(&runtime->stream);
    if (runtime->credential.data &&
        secure_buffer_transfer(&returned_profile->credential, &runtime->credential)) {
        returned_profile->metadata = runtime->user->metadata;
    } else {
        secure_buffer_release(&runtime->credential);
        value.error = GatewayClientError::invalid_input;
    }
    release_runtime_allocation(client);
    reset_value<GatewayClient>(client);
    value.phase =
        value.error == GatewayClientError::none ? GatewayClientPhase::stopped : GatewayClientPhase::failed;
    return value;
}

AccountRequirement gateway_client_required_action(const GatewayClient* client) noexcept {
    return client ? static_cast<AccountRequirement>(ReadAcquire(&client->required_action))
                  : AccountRequirement::none;
}

GatewayClientResult gateway_client_status(const GatewayClient* client) noexcept {
    return result(client);
}

GatewayClientPopResult gateway_client_pop_event_payload(GatewayClient* client, GatewayClientEvent* event,
                                                        Slice<u8> payload_output) noexcept {
    if (!client || !client->runtime || !event || (!payload_output.data && payload_output.count != 0)) {
        return GatewayClientPopResult{0, GatewayClientPopError::invalid_input};
    }
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    const u32 before = spsc_mailbox_load_u32(&runtime->events.read_sequence);
    // Only after the head descriptor is visible: the worker sets the lease
    // (and its committed bytes) before it publishes an external record.
    const ConstSlice<u8> large =
        spsc_mailbox_head_external(runtime->events) && ReadAcquire(&runtime->large_record_lease)
            ? ConstSlice<u8>{runtime->large_record.base, runtime->large_record.committed}
            : ConstSlice<u8>{};
    const SpscMailboxResult popped = spsc_mailbox_pop(&runtime->events, event, payload_output, large);
    if (popped.ok() && popped.external)
        release_large_record(runtime);
    if (spsc_mailbox_load_u32(&runtime->events.read_sequence) != before)
        gateway_mailbox_signal_space(&runtime->events, runtime->mailbox_space);
    GatewayClientPopError error = GatewayClientPopError::none;
    switch (popped.error) {
    case SpscMailboxError::none:
        break;
    case SpscMailboxError::empty:
        error = GatewayClientPopError::empty;
        break;
    case SpscMailboxError::output_too_small:
        error = GatewayClientPopError::output_too_small;
        break;
    case SpscMailboxError::corrupt:
        error = GatewayClientPopError::corrupt;
        break;
    default:
        error = GatewayClientPopError::invalid_input;
        break;
    }
    return GatewayClientPopResult{popped.payload_bytes, error};
}

HANDLE gateway_client_notification_handle(const GatewayClient* client) noexcept {
    return client ? client->notification_event : nullptr;
}

bool gateway_client_snapshot_user_session(const GatewayClient* client,
                                          GatewayUserSessionSnapshot* session) noexcept {
    if (!client || !client->runtime || !session) {
        return false;
    }
    const auto* runtime = static_cast<const GatewayClientRuntime*>(client->runtime);
    if (!runtime->user || runtime->user->metadata.profile_id == 0) {
        return false;
    }
    AcquireSRWLockShared(const_cast<SRWLOCK*>(&runtime->resume_lock));
    GatewayResumeState resume = runtime->resume;
    // The lock pairs the session with its READY; the worker advances the sequence without it.
    resume.sequence = static_cast<u64>(ReadAcquire64(&runtime->resume_sequence));
    const u64 self_user_id = runtime->resume_user_id;
    ReleaseSRWLockShared(const_cast<SRWLOCK*>(&runtime->resume_lock));
    GatewayUserSessionSnapshot snapshot{};
    snapshot.profile_id = runtime->user->metadata.profile_id;
    if (resume.valid) {
        if (!gateway_resume_validate(resume)) {
            return false;
        }
        snapshot.resume = resume;
        snapshot.self_user_id = self_user_id;
        snapshot.has_resume = true;
    }
    *session = snapshot;
    return true;
}

bool gateway_client_subscribe_guild_members(GatewayClient* client,
                                            const GatewayGuildSubscription& subscription) noexcept {
    auto* runtime = live_user_runtime(client);
    if (!runtime || subscription.guild_id == 0 || subscription.clear_thread_guild_id != 0) {
        return false;
    }
    u8 validation[kGatewayUserMaximumCommandBytes];
    const GatewayCommandWriteResult valid = gateway_write_guild_subscriptions_bulk(
        subscription, Slice<u8>{validation, kGatewayUserMaximumCommandBytes});
    SecureZeroMemory(validation, sizeof(validation));
    if (!valid.ok()) {
        return false;
    }
    // A members-only frame (the users followed in a guild) is independent of the viewport frame it travels
    // beside: each is last-write-wins in its own slot, so neither can overwrite the other before it is sent.
    const bool members_only = subscription.members_present && subscription.omit_channel_ranges &&
                              subscription.omit_features && !subscription.thread_member_lists_present;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    copy_value(members_only ? &runtime->desired_members : &runtime->desired_subscription, subscription);
    u32& revision = members_only ? runtime->members_revision : runtime->subscription_revision;
    u32& sent = members_only ? runtime->sent_members_revision : runtime->sent_subscription_revision;
    if (++revision == 0) {
        revision = 1;
        sent = 0;
    }
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    // A concurrent already-pending post also guarantees a worker wake and the
    // desired state is last-write-wins under subscription_lock.
    return wake_worker(client, &runtime->subscription_signal);
}

bool gateway_client_request_guild_members(GatewayClient* client,
                                          const GatewayGuildMemberRequest& request) noexcept {
    auto* runtime = live_user_runtime(client);
    if (!runtime || !gateway_guild_member_request_valid(request))
        return false;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    const auto prior = runtime->member_requests;
    bool admitted = gateway_member_request_admit(&runtime->member_requests, request, GetTickCount64());
    if (admitted && !wake_worker(client, &runtime->subscription_signal)) {
        runtime->member_requests = prior;
        admitted = false;
    }
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    return admitted;
}

bool gateway_client_request_channel_info(GatewayClient* client, u64 guild_id) noexcept {
    auto* runtime = live_user_runtime(client);
    if (!runtime || !guild_id)
        return false;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    bool admitted = gateway_channel_info_admit(&runtime->channel_info, guild_id);
    if (admitted && !wake_worker(client, &runtime->subscription_signal)) {
        gateway_channel_info_unqueue(&runtime->channel_info, guild_id);
        admitted = false;
    }
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    return admitted;
}

void gateway_client_cancel_guild_member_request(GatewayClient* client) noexcept {
    if (!client || !client->runtime)
        return;
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    gateway_member_request_cancel_batch(&runtime->member_requests);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
}

void gateway_client_cancel_guild_member_query(GatewayClient* client) noexcept {
    if (!client || !client->runtime)
        return;
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    gateway_member_request_cancel_query(&runtime->member_requests);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
}

GatewayClientSelfStatusSnapshot gateway_client_self_status_snapshot(const GatewayClient* client) noexcept {
    GatewayClientSelfStatusSnapshot result{};
    auto* runtime = user_runtime(client);
    if (!runtime)
        return result;
    AcquireSRWLockShared(&runtime->subscription_lock);
    const auto& owner = runtime->self_status;
    result.profile_id = owner.profile_id;
    result.self_user_id = owner.self_user_id;
    result.settings_revision = owner.settings_revision;
    result.status = owner.desired.status;
    result.confirmed_status = owner.confirmed_status;
    result.last_error = runtime->self_status_error;
    result.authoritative = owner.authoritative;
    result.available = owner.self_user_id && load_phase(client) == GatewayClientPhase::ready;
    result.pending = owner.request_id || (owner.authoritative && gateway_self_status_pending(owner));
    result.revision =
        owner.revision_serial * 4 + (result.authoritative ? 1 : 0) + (result.available ? 2 : 0);
    ReleaseSRWLockShared(&runtime->subscription_lock);
    return result;
}

namespace {

// Sends the self-status settings-proto/1 GET (fetch) or PATCH (update) with the
// subscription lock held, and records an accepted request on the owner.
RestClientSubmitResult submit_self_settings_locked(GatewayClientRuntime* runtime,
                                                   RestClient* rest,
                                                   SelfStatusSettingsOperation operation,
                                                   const ConstSlice<u8>& body, u64 request_id) noexcept {
    const bool update = operation == SelfStatusSettingsOperation::update;
    static constexpr char target[] = "/api/v10/users/@me/settings-proto/1";
    const RestClientRequest rest_request{{reinterpret_cast<const u8*>(target), sizeof(target) - 1},
                                         body,
                                         {0x5354415455535052ull, 0,
                                          update ? RestMethod::patch : RestMethod::get,
                                          RestMajorParameter::none},
                                         request_id,
                                         kRestClientDefaultTimeoutMs,
                                         update};
#if defined(MAICORD_GATEWAY_SELF_STATUS_TEST)
    const auto submitted = self_status_test_submit ? self_status_test_submit(rest_request)
                                                   : rest_client_submit(rest, rest_request);
#else
    const auto submitted = rest_client_submit(rest, rest_request);
#endif
    if (submitted.ok()) {
        auto& owner = runtime->self_status;
        owner.request_id = request_id;
        owner.request_settings_revision = owner.settings_revision;
        ++owner.revision_serial;
        runtime->self_status_error = GatewaySelfStatusError::none;
    }
    return submitted;
}

// A settings-proto PATCH that rewrites one part of the observed status group: `encode(argument, group,
// scratch, body)` writes it under the same lock as the checks and the enqueue.
using StatusGroupEncode = GatewayCommandWriteResult (*)(const void* argument, ConstSlice<u8> group,
                                                        Slice<u8> scratch, Slice<u8> body) noexcept;
RestClientSubmitResult submit_status_group_patch(GatewayClient* client, RestClient* rest, u64 profile,
                                                 u64 self, u64 request_id, StatusGroupEncode encode,
                                                 const void* argument) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = user_runtime(client);
    if (!runtime || !rest || !request_id || !profile || !self ||
        load_phase(client) != GatewayClientPhase::ready)
        return invalid;
    u8 scratch[kSelfStatusSettingsMaximumGroupBytes + 704];
    u8 body[kSelfStatusSettingsMaximumPatchBytes + 1024];
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    const auto& owner = runtime->self_status;
    if (load_phase(client) != GatewayClientPhase::ready || owner.profile_id != profile ||
        owner.self_user_id != self || owner.request_id || !owner.authoritative) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return invalid;
    }
    const auto encoded =
        encode(argument, ConstSlice<u8>{runtime->self_status_group, runtime->self_status_group_bytes},
               Slice<u8>{scratch, sizeof(scratch)}, Slice<u8>{body, sizeof(body)});
    if (!encoded.ok()) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return invalid;
    }
    const auto submitted = submit_self_settings_locked(runtime, rest, SelfStatusSettingsOperation::update,
                                                       {body, encoded.bytes}, request_id);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    SecureZeroMemory(scratch, sizeof(scratch));
    SecureZeroMemory(body, sizeof(body));
    return submitted;
}

} // namespace

RestClientSubmitResult gateway_client_self_status_submit(GatewayClient* client,
                                                         RestClient* rest, u64 profile,
                                                         u64 self,
                                                         SelfStatusSettingsOperation operation,
                                                         GatewayUserStatus status, u64 request_id,
                                                         u64 unix_now_ms) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    auto* runtime = user_runtime(client);
    if (!runtime || !rest || !request_id || !profile || !self ||
        operation > SelfStatusSettingsOperation::update || status > GatewayUserStatus::invisible ||
        load_phase(client) != GatewayClientPhase::ready)
        return invalid;
    // Submit copies synchronously; hold one existing lock through validation,
    // body construction and enqueueing so a remote update cannot race the patch.
    u8 scratch[kSelfStatusSettingsMaximumGroupBytes + 64];
    u8 body[kSelfStatusSettingsMaximumPatchBytes];
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    auto& owner = runtime->self_status;
    if (load_phase(client) != GatewayClientPhase::ready || owner.profile_id != profile ||
        owner.self_user_id != self || owner.request_id ||
        (operation == SelfStatusSettingsOperation::update && !owner.authoritative)) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return invalid;
    }
    GatewayCommandWriteResult encoded{};
    if (operation == SelfStatusSettingsOperation::update) {
        encoded = self_status_settings_write_patch(
            {runtime->self_status_group, runtime->self_status_group_bytes}, status, unix_now_ms,
            {scratch, sizeof(scratch)}, {body, sizeof(body)});
        if (!encoded.ok()) {
            ReleaseSRWLockExclusive(&runtime->subscription_lock);
            return invalid;
        }
    }
    const auto submitted =
        submit_self_settings_locked(runtime, rest, operation, {body, encoded.bytes}, request_id);
    if (submitted.ok() && operation == SelfStatusSettingsOperation::update)
        gateway_self_status_request(&owner, profile, self, status);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    SecureZeroMemory(scratch, sizeof(scratch));
    SecureZeroMemory(body, sizeof(body));
    if (submitted.ok())
        post_subscription_change(client, runtime);
    return submitted;
}
bool gateway_client_self_status_complete(GatewayClient* client, u64 profile, u64 self, u64 request_id,
                                         const RestClientResponse* response,
                                         Utf8View body) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !response || !request_id || response->request_id != request_id)
        return false;
    SelfSettingsJson parsed{};
    const bool success = response->transport_ok() && response->status_code >= 200 &&
                         response->status_code < 300 && self_settings_json(body, &parsed) &&
                         (parsed.seen & 1) && !parsed.out_of_date;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    auto& owner = runtime->self_status;
    // A disconnected worker does not invalidate the primary REST request ID. The
    // UI observer contract excludes stop/join; account/request ID guards
    // allow its completion to be consumed without making controls available during reconnect.
    if (owner.profile_id != profile || owner.self_user_id != self || owner.request_id != request_id) {
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return false;
    }
    owner.request_id = 0;
    ++owner.revision_serial;
    if (owner.settings_revision == owner.request_settings_revision) {
        if (success)
            self_settings_observe_locked(runtime, parsed.encoded, false);
        else {
            runtime->self_status_error = parsed.out_of_date          ? GatewaySelfStatusError::out_of_date
                                         : !response->transport_ok() ? GatewaySelfStatusError::transport
                                                                     : GatewaySelfStatusError::rejected;
            if (owner.authoritative && owner.desired.status != owner.confirmed_status) {
                owner.desired = {0, owner.confirmed_status, false, false};
                if (++owner.revision == 0)
                    owner.revision = 1;
            }
        }
    }
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    post_subscription_change(client, runtime);
    return true;
}

bool gateway_client_custom_status_snapshot(const GatewayClient* client, SelfCustomStatus* output) noexcept {
    if (!output)
        return false;
    reset_value(output);
    auto* runtime = user_runtime(client);
    if (!runtime)
        return false;
    AcquireSRWLockShared(&runtime->subscription_lock);
    const bool known = runtime->self_status.authoritative && runtime->self_status_group &&
                       self_status_settings_read_custom({runtime->self_status_group, runtime->self_status_group_bytes},
                                                        output);
    ReleaseSRWLockShared(&runtime->subscription_lock);
    return known;
}
RestClientSubmitResult gateway_client_custom_status_submit(GatewayClient* client, RestClient* rest,
                                                           u64 profile, u64 self,
                                                           const SelfCustomStatus* custom,
                                                           u64 request_id) noexcept {
    return submit_status_group_patch(
        client, rest, profile, self, request_id,
        [](const void* argument, ConstSlice<u8> group, Slice<u8> scratch, Slice<u8> body) noexcept {
            return self_status_settings_write_custom_patch(
                group, static_cast<const SelfCustomStatus*>(argument), scratch, body);
        },
        custom);
}

bool gateway_client_local_presence(GatewayClient* client, const GatewayLocalPresence* input,
                                   GatewayActivitySharing* output) noexcept {
    if (output)
        reset_value(output);
    auto* runtime = user_runtime(client);
    if (!runtime || (input && !gateway_local_presence_valid(*input)))
        return false;
    bool changed = false;
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    auto& owner = runtime->self_status;
    if (input && !gateway_local_presence_equal(runtime->local_presence, *input)) {
        copy_value(&runtime->local_presence, *input);
        if (++owner.revision == 0)
            owner.revision = 1;
        changed = true;
    }
    if (output) {
        output->known = owner.authoritative && runtime->self_status_group &&
                        self_status_settings_read_show_game(
                            {runtime->self_status_group, runtime->self_status_group_bytes}, &output->share);
        if (!output->known)
            output->share = false;
    }
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    if (changed)
        post_subscription_change(client, runtime);
    return true;
}
void gateway_client_activity_changed(GatewayClient* client) noexcept {
    if (user_runtime(client))
        network_signal_post(&client->worker.loop,
                            &static_cast<GatewayClientRuntime*>(client->runtime)->subscription_signal, 1);
}
RestClientSubmitResult gateway_client_activity_sharing_submit(GatewayClient* client, RestClient* rest,
                                                              u64 profile, u64 self, bool share,
                                                              u64 request_id) noexcept {
    return submit_status_group_patch(
        client, rest, profile, self, request_id,
        [](const void* argument, ConstSlice<u8> group, Slice<u8> scratch, Slice<u8> body) noexcept {
            return self_status_settings_write_show_game_patch(group, *static_cast<const bool*>(argument),
                                                              scratch, body);
        },
        &share);
}

#if defined(MAICORD_GATEWAY_SELF_STATUS_TEST)
bool gateway_self_status_test_create(GatewayClient* client, u64 profile, u64 self,
                                     GatewaySelfStatusTestSubmit submit) noexcept {
    if (!client || client->runtime || !profile || !self || !submit)
        return false;
    auto* runtime = static_cast<GatewayClientRuntime*>(
        VirtualAlloc(nullptr, kGatewayUserClientRuntimeBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!runtime)
        return false;
    client->runtime = runtime;
    runtime->user =
        reinterpret_cast<GatewayUserRuntime*>(reinterpret_cast<u8*>(runtime) + kGatewayUserRuntimeOffset);
    runtime->user->metadata.profile_id = profile;
    runtime->session.lifecycle.phase = GatewayLifecyclePhase::ready;
    runtime->self_user_id = self;
    gateway_self_status_initialize(&runtime->self_status, profile,
                                   {0, GatewayUserStatus::online, false, false});
    gateway_self_status_bind(&runtime->self_status, self);
    client->phase = static_cast<LONG>(GatewayClientPhase::ready);
    self_status_test_submit = submit;
    return true;
}
bool gateway_self_status_test_observe(GatewayClient* client, Utf8View dispatch) noexcept {
    if (!client || !client->runtime)
        return false;
    return self_settings_dispatch(client, static_cast<GatewayClientRuntime*>(client->runtime), dispatch);
}

void gateway_self_status_test_ready(GatewayClient* client, bool ready) noexcept {
    if (!client || !client->runtime)
        return;
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    gateway_self_status_reset_connection(&runtime->self_status);
    runtime->session.lifecycle.phase =
        ready ? GatewayLifecyclePhase::ready : GatewayLifecyclePhase::awaiting_hello;
    client->phase =
        static_cast<LONG>(ready ? GatewayClientPhase::ready : GatewayClientPhase::reconnect_wait);
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
}
u32 gateway_self_status_test_group_bytes(const GatewayClient* client) noexcept {
    return client && client->runtime
               ? static_cast<const GatewayClientRuntime*>(client->runtime)->self_status_group_bytes
               : 0;
}
u32 gateway_self_status_test_destroy(GatewayClient* client) noexcept {
    if (!client || !client->runtime)
        return 0;
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    u32 wiped = 0;
    if (runtime->self_status_group) {
        SecureZeroMemory(runtime->self_status_group, kSelfStatusSettingsMaximumGroupBytes);
        for (u32 i = 0; i < kSelfStatusSettingsMaximumGroupBytes; ++i)
            if (!runtime->self_status_group[i])
                ++wiped;
        VirtualFree(runtime->self_status_group, 0, MEM_RELEASE);
    }
    SecureZeroMemory(runtime, kGatewayUserClientRuntimeBytes);
    VirtualFree(runtime, 0, MEM_RELEASE);
    reset_value(client);
    self_status_test_submit = nullptr;
    return wiped;
}
#endif

bool gateway_client_request_voice_state(GatewayClient* client,
                                        const GatewayVoiceStateUpdate& state) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !client->worker.thread || load_phase(client) != GatewayClientPhase::ready)
        return false;
    AcquireSRWLockExclusive(&runtime->voice_lock);
    if (!runtime->voice_snapshot.user_id) {
        ReleaseSRWLockExclusive(&runtime->voice_lock);
        return false;
    }
    const auto previous = runtime->desired_voice;
    const bool was_pending = runtime->voice_request_pending;
    runtime->desired_voice = state;
    runtime->voice_request_pending = true;
    const bool posted = wake_worker(client, &runtime->voice_signal);
    if (!posted) {
        runtime->desired_voice = previous;
        runtime->voice_request_pending = was_pending;
    }
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    return posted;
}

bool gateway_client_voice_snapshot(const GatewayClient* client,
                                   GatewayClientVoiceSnapshot* output) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !output)
        return false;
    AcquireSRWLockShared(&runtime->voice_lock);
    *output = runtime->voice_snapshot;
    ReleaseSRWLockShared(&runtime->voice_lock);
    return true;
}

bool gateway_client_voice_user_audio(GatewayClient* client, u64 user_id, u32 gain_percent, bool muted) noexcept {
    auto* runtime = user_runtime(client);
    // Settings live beside the playback lock, so this is safe from the UI thread.
    return runtime && voice_receive_set_user(&runtime->voice_receive, user_id, gain_percent, muted);
}

bool gateway_client_voice_audio_configure(GatewayClient* client, const VoiceAudioSettings& settings) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !client->worker.thread)
        return false;
    voice_send_configure(&runtime->voice_send, settings);
    wake_worker(client, &runtime->voice_send_signal);
    return true;
}

bool gateway_client_voice_audio_status(GatewayClient* client, VoiceAudioStatus* output) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !output)
        return false;
    *output = voice_send_status(&runtime->voice_send);
    return true;
}

bool gateway_client_voice_video_snapshot(const GatewayClient* client,
                                         GatewayClientVoiceVideoSnapshot* output) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !output)
        return false;
    AcquireSRWLockShared(&runtime->voice_lock);
    copy_value(output, runtime->voice_video_snapshot);
    ReleaseSRWLockShared(&runtime->voice_lock);
    return true;
}

bool gateway_client_stream_watch(GatewayClient* client, const GatewayStreamKey& key) noexcept {
    auto* runtime = user_runtime(client);
    const bool stop = key.type == GatewayStreamType::none;
    if (!runtime || !client->worker.thread || load_phase(client) != GatewayClientPhase::ready ||
        (!stop && !gateway_stream_key_valid(key)))
        return false;
    AcquireSRWLockExclusive(&runtime->voice_lock);
    const auto& voice = runtime->voice_snapshot;
    // Watching needs the local call in the stream's channel; the worker
    // re-checks and ends the stream when the call leaves it.
    if (!voice.user_id || (!stop && (voice.phase == VoiceSetupPhase::idle || voice.channel_id != key.channel_id ||
                                     key.owner_id == voice.user_id))) {
        ReleaseSRWLockExclusive(&runtime->voice_lock);
        return false;
    }
    const auto previous = runtime->desired_stream;
    const bool was_pending = runtime->stream_request_pending;
    runtime->desired_stream = stop ? GatewayStreamKey{} : key;
    runtime->stream_request_pending = true;
    const bool posted = wake_worker(client, &runtime->voice_signal);
    if (!posted) {
        runtime->desired_stream = previous;
        runtime->stream_request_pending = was_pending;
    }
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    return posted;
}

bool gateway_client_stream_snapshot(const GatewayClient* client, GatewayClientStreamSnapshot* output) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !output)
        return false;
    AcquireSRWLockShared(&runtime->voice_lock);
    *output = runtime->stream_snapshot;
    ReleaseSRWLockShared(&runtime->voice_lock);
    return true;
}

bool gateway_client_stream_volume(GatewayClient* client, u32 percent) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || percent > kGatewayStreamVolumeMaximum)
        return false;
    AcquireSRWLockExclusive(&runtime->voice_lock);
    runtime->desired_stream_volume = percent;
    runtime->stream_volume_pending = true;
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    wake_worker(client, &runtime->voice_signal);
    return true;
}

bool gateway_client_voice_video_wanted(GatewayClient* client, u64 user_id, bool wanted) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !client->worker.thread || !user_id)
        return false;
    bool ok = true;
    AcquireSRWLockExclusive(&runtime->voice_lock);
    if (gateway_stream_video_user_tagged(user_id)) {
        // The watched stream's tile: its own connection's op 15.
        const u64 owner = gateway_stream_video_owner(user_id);
        if (wanted ? runtime->stream_unwanted_owner == owner : runtime->stream_unwanted_owner != owner) {
            runtime->stream_unwanted_owner = wanted ? 0 : owner;
            runtime->stream_wants_dirty = true;
        }
        ReleaseSRWLockExclusive(&runtime->voice_lock);
        wake_worker(client, &runtime->voice_send_signal);
        return owner != 0;
    }
    u32 at = 0;
    while (at != runtime->video_unwanted_count && runtime->video_unwanted[at] != user_id)
        ++at;
    if (wanted && at != runtime->video_unwanted_count)
        runtime->video_unwanted[at] = runtime->video_unwanted[--runtime->video_unwanted_count];
    else if (!wanted && at == runtime->video_unwanted_count) {
        ok = at != kVoiceVideoUnwantedUsers;
        if (ok)
            runtime->video_unwanted[runtime->video_unwanted_count++] = user_id;
    }
    runtime->video_unwanted_dirty = true;
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    if (ok)
        wake_worker(client, &runtime->voice_send_signal);
    return ok;
}

GatewayStateRecoveryResult gateway_client_request_fresh_state(GatewayClient* client) noexcept {
    if (!client || !client->runtime || !client->worker.thread)
        return GatewayStateRecoveryResult::unavailable;
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    auto result = gateway_state_recovery_request(&runtime->state_recovery,
                                                 load_phase(client) == GatewayClientPhase::ready);
    if (result == GatewayStateRecoveryResult::queued &&
        !wake_worker(client, &runtime->subscription_signal)) {
        gateway_state_recovery_cancel_post(&runtime->state_recovery);
        result = GatewayStateRecoveryResult::unavailable;
    }
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    return result;
}

bool gateway_client_set_video_frames(GatewayClient* client, const GatewayVideoFrames& frames) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !client->worker.thread)
        return false;
    AcquireSRWLockExclusive(&runtime->voice_lock);
    runtime->video_frames = frames;
    runtime->video_frames_dirty = true;
    ReleaseSRWLockExclusive(&runtime->voice_lock);
    wake_worker(client, &runtime->voice_send_signal);
    return true;
}

bool gateway_client_has_video_frames(const GatewayClient* client, const void* context) noexcept {
    if (!client || !client->runtime)
        return false;
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    AcquireSRWLockShared(&runtime->voice_lock);
    const bool installed = runtime->video_frames.context == context;
    ReleaseSRWLockShared(&runtime->voice_lock);
    return installed;
}

bool gateway_client_attach_soundboard(GatewayClient* client, SoundboardMixer* mixer) noexcept {
    auto* runtime = user_runtime(client);
    if (!runtime || !client->worker.thread)
        return false;
    auto& playback = runtime->voice_receive.playback;
    if (voice_playback_effects(&playback) != mixer)
        voice_playback_set_effects(&playback, mixer);
    AcquireSRWLockShared(playback.lock.native());
    const bool live = playback.block != nullptr;
    ReleaseSRWLockShared(playback.lock.native());
    return live;
}

} // namespace maicord
