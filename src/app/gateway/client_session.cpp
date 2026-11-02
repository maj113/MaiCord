#include "client_internal.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/platform/crypto/random.hpp"
#include "../../protocol/encoding/json_field_lists.hpp"

namespace maicord {
namespace {

// Out of line: its resume capture would otherwise join process_session's stack frame. Once per session:
// compiled for size.
// receive means the READY was handled and the session loop continues.
__declspec(noinline) __attribute__((minsize)) PlaintextDrive process_ready(GatewayClient* client,
                                                                          GatewayClientRuntime* runtime,
                                                                          const GatewaySessionEvent& event,
                                                                          u64 now_ms) noexcept {
    // One deadline spans the whole READY projection. No lock is
    // held while producer backpressure waits for the UI consumer.
    runtime->mailbox_deadline_ms = GetTickCount64() + kPhaseTimeoutMs;
    if (!gateway_client_ready_matches_identity(runtime->identity, event.ready)) {
        return fail_plaintext(runtime, GatewayClientFailureStage::ready_identity);
    }
    // A READY without one drops the stored data, as the client does.
    JsonString qos{};
    GatewayUserRuntime& user = *runtime->user;
    user.has_qos_derived =
        json_value_as_string(event.ready.qos_token, &qos) && !qos.has_escapes &&
        gateway_qos_derived_from_ready(qos.encoded, {user.qos_derived, kGatewayQosDerivedMaximumBytes},
                                       &user.qos_derived_bytes);
    GatewayResumeState captured_state{};
    const GatewayResumeCaptureError captured = gateway_resume_capture(
        &captured_state, event.ready, runtime->session.lifecycle.last_sequence);
    if (captured != GatewayResumeCaptureError::none) {
        return fail_plaintext(runtime, GatewayClientFailureStage::ready_resume_capture);
    }
    ReadyProjectionBinding binding{client, runtime, event.ready.required_action};
    // The envelope's index stays intact (gateway_decode_ready read through it) while the projection indexes its
    // arrays into their own storage: every scan of a large container inside READY is then answered from it.
    Slice<u32> arrays = main_index_view(runtime);
    const JsonIndexedDocument& document = event.envelope.document;
    ReadyScanSource source;
    if (document.structurals.data == arrays.data && document.structurals.count &&
        stable_storage_grow(&runtime->ready_arrays, arrays.count)) {
        arrays = ready_arrays_view(runtime);
        source.set(document);
    }
    // The zero-copy READY packet stays in the current session message
    // backing while both passes run. No push/next/compaction occurs
    // until all typed routes validate and the commit has finished.
    const auto projection = gateway_ready_project(
        event.ready,
        {arrays,
         ready_record_index_view(runtime),
         dispatch_scratch(runtime),
         {reinterpret_cast<char*>(runtime->gateway_payload), sizeof(runtime->gateway_payload)},
         grow_ready_record_index,
         runtime,
         {acquire_large_record, runtime},
         runtime->ready_staging.base ? &runtime->ready_staging : nullptr},
        captured_state.sequence, now_ms, publish_ready_projection, ready_projection_work, &binding);
    settle_large_record(runtime);
    if (!projection.ok()) {
        note_route_error(runtime, projection.route);
        const GatewayClientFailureStage stage =
            projection.failure_stage == GatewayClientFailureStage::none ? GatewayClientFailureStage::ready_route
                                                                        : projection.failure_stage;
        // An accepted READY prefix may already have replaced model
        // state. No prior resume checkpoint can certify that prefix,
        // so the next attempt is always a fresh IDENTIFY.
        if (projection.publication_started)
            clear_resume(runtime);
        // Owner shutdown (stop or mailbox cancel) ends the worker as before.
        if (runtime->stopping || WaitForSingleObject(runtime->mailbox_cancel, 0) == WAIT_OBJECT_0)
            return fail_plaintext(runtime, stage);
        // A READY that cannot be projected is not an authentication
        // failure: report the categorical reason, keep the session and
        // identify again later. The growing floor keeps a READY that
        // fails the same way every time far below the server's daily
        // IDENTIFY budget.
        GatewayClientEvent diagnostic{};
        diagnostic.timestamp_ms = now_ms;
        diagnostic.kind = GatewayClientEventKind::reconnecting;
        diagnostic.error = GatewayClientError::session;
        diagnostic.failure_stage = stage;
        note_route_error(&diagnostic, projection.route);
        publish_event(runtime, diagnostic);
        u32 random = 0;
        if (!secure_random_u32(&random).ok())
            return fail_plaintext(runtime, stage);
        const u32 failures = runtime->ready_failures < 16 ? ++runtime->ready_failures : 16;
        const u32 floor_ms = gateway_ready_failure_delay(failures);
        const u32 jitter_ms = gateway_reconnect_next(&runtime->reconnect_backoff, random).delay_ms;
        return schedule_reconnect(client, runtime, GatewayLifecycleAction::reconnect_identify, now_ms,
                                  floor_ms + jitter_ms)
                   ? PlaintextDrive::reconnecting
                   : fail_plaintext(runtime, stage);
    }
    runtime->ready_failures = 0;
    if (projection.degraded_guilds) {
        // Guilds left unavailable are reported with their first
        // categorical failure; the rest of READY was applied.
        GatewayClientEvent degraded{};
        degraded.timestamp_ms = now_ms;
        degraded.kind = GatewayClientEventKind::dispatch;
        degraded.dispatch_kind = GatewayClientDispatchKind::unknown;
        degraded.failure_stage = GatewayClientFailureStage::ready_guild_route;
        note_route_error(&degraded, projection.degraded_route);
        if (degraded.event_decode_error == GatewayEventDecodeError::none)
            degraded.event_decode_error = GatewayEventDecodeError::invalid_data;
        degraded.native_error = projection.degraded_guilds;
        degraded.sequence = captured_state.sequence;
        degraded.has_sequence = true;
        publish_event_payload(client, runtime, degraded, {});
    }
    runtime->mailbox_deadline_ms = 0;
    // Save the resume token only after the READY projection has
    // entered the consumer queue. Saving it before routing can persist
    // a session whose model never existed, so the next process would
    // resume into an empty shell.
    publish_resume(runtime, captured_state);
    AcquireSRWLockExclusive(&runtime->subscription_lock);
    gateway_state_recovery_ready(&runtime->state_recovery);
    gateway_subscription_thread_owner_ready(&runtime->thread_subscription_owner, true);
    gateway_channel_info_reset_session(&runtime->channel_info);
    if (gateway_self_status_bind(&runtime->self_status, event.ready.user_id))
        self_settings_observe_locked(runtime, event.ready.user_settings_proto, false);
    else {
        runtime->self_status.authoritative = false;
        runtime->self_status_error = GatewaySelfStatusError::settings;
        ++runtime->self_status.revision_serial;
    }
    runtime->sent_subscription_revision = 0;
    runtime->sent_members_revision = 0;
    runtime->active_subscription_revision = 0;
    ReleaseSRWLockExclusive(&runtime->subscription_lock);
    gateway_reconnect_reset(&runtime->reconnect_backoff);
    runtime->deadline_ms = 0;
    InterlockedIncrement64(&client->frecency_settings_revision);
    publish_phase(client, GatewayClientPhase::ready);
    client_session_ready(runtime);
    return PlaintextDrive::receive;
}

PlaintextDrive process_session(GatewayClient* client, GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    for (;;) {
        trim_message_banks(runtime);
        // Reuse the owner's deadline field during bounded envelope rescans;
        // no index growth can extend this absolute budget.
        runtime->mailbox_deadline_ms = GetTickCount64() + kPhaseTimeoutMs;
        GatewaySessionEvent event;
        gateway_session_next(&event, &runtime->session, runtime->heartbeat_jitter, now_ms);
        runtime->mailbox_deadline_ms = 0;
        if (event.kind == GatewaySessionEventKind::need_input) {
            return PlaintextDrive::receive;
        }
        if (event.kind == GatewaySessionEventKind::opened || event.kind == GatewaySessionEventKind::pong) {
            if (event.kind == GatewaySessionEventKind::opened) {
                runtime->last_ack_ms = now_ms;
                GatewayClientEvent opened{};
                opened.timestamp_ms = now_ms;
                opened.kind = GatewayClientEventKind::websocket_opened;
                publish_event(runtime, opened);
            }
            continue;
        }
        if (event.kind == GatewaySessionEventKind::ping) {
            if (!queue_pong(runtime, event.control_payload)) {
                return fail_plaintext(runtime, GatewayClientFailureStage::pong_queue);
            }
            if (runtime->send_purpose == SendPurpose::none) {
                return send_pending_pong(runtime)
                           ? PlaintextDrive::sending
                           : fail_plaintext(runtime, GatewayClientFailureStage::pong_send);
            }
            continue;
        }
        if (event.kind == GatewaySessionEventKind::hello && event.hello.heartbeat_interval_ms != 0) {
            GatewayClientEvent hello{};
            hello.timestamp_ms = now_ms;
            hello.heartbeat_interval_ms = event.hello.heartbeat_interval_ms;
            hello.kind = GatewayClientEventKind::hello;
            publish_event(runtime, hello);
            InterlockedExchange64(&client->hello_elapsed_us,
                                  static_cast<LONG64>(clock_microseconds(
                                      runtime->clock, runtime->start_counter, clock_now())));
            if (!runtime->authenticated) {
                publish_phase(client, GatewayClientPhase::hello);
                return PlaintextDrive::hello;
            }

            const GatewayLifecycleAction session_action = event.lifecycle_step.action;
            if (runtime->session.lifecycle.next_heartbeat_ms <= now_ms) {
                const GatewayLifecycleStep heartbeat =
                    gateway_lifecycle_tick(&runtime->session.lifecycle, now_ms);
                if (!heartbeat.ok() || heartbeat.action != GatewayLifecycleAction::send_heartbeat ||
                    !gateway_outbound_enqueue(&runtime->outbound, heartbeat.action)) {
                    return fail_plaintext(runtime, GatewayClientFailureStage::hello_heartbeat);
                }
            }
            if ((session_action != GatewayLifecycleAction::send_identify &&
                 session_action != GatewayLifecycleAction::send_resume) ||
                !gateway_outbound_enqueue(&runtime->outbound, session_action)) {
                return fail_plaintext(runtime, GatewayClientFailureStage::hello_action);
            }
            runtime->deadline_ms = now_ms + kPhaseTimeoutMs;
            return drive_queued_actions(client, runtime, now_ms);
        }
        if (event.kind == GatewaySessionEventKind::heartbeat_requested) {
            if (event.lifecycle_step.action != GatewayLifecycleAction::send_heartbeat ||
                !gateway_outbound_enqueue(&runtime->outbound, event.lifecycle_step.action)) {
                return fail_plaintext(runtime, GatewayClientFailureStage::heartbeat_request);
            }
            return drive_queued_actions(client, runtime, now_ms);
        }
        if (event.kind == GatewaySessionEventKind::heartbeat_acknowledged) {
            GatewayClientEvent acknowledged{};
            acknowledged.timestamp_ms = now_ms;
            acknowledged.kind = GatewayClientEventKind::heartbeat_acknowledged;
            runtime->last_ack_ms = now_ms;
            publish_event(runtime, acknowledged);
            continue;
        }
        if (event.kind == GatewaySessionEventKind::ready) {
            const PlaintextDrive handled = process_ready(client, runtime, event, now_ms);
            if (handled != PlaintextDrive::receive)
                return handled;
            continue;
        }
        if (event.kind == GatewaySessionEventKind::resumed) {
            InterlockedIncrement64(&client->frecency_settings_revision);
            AcquireSRWLockExclusive(&runtime->subscription_lock);
            gateway_channel_info_reset_session(&runtime->channel_info);
            ReleaseSRWLockExclusive(&runtime->subscription_lock);
            gateway_subscription_thread_owner_ready(&runtime->thread_subscription_owner, false);
            publish_voice_snapshot(client, runtime);
            runtime->deadline_ms = 0;
            gateway_reconnect_reset(&runtime->reconnect_backoff);
            GatewayClientEvent resumed{};
            resumed.timestamp_ms = now_ms;
            resumed.kind = GatewayClientEventKind::resumed;
            resumed.sequence = runtime->session.lifecycle.last_sequence;
            resumed.has_sequence = runtime->session.lifecycle.has_sequence;
            publish_event(runtime, resumed);
            publish_phase(client, GatewayClientPhase::ready);
            continue;
        }
        if (event.kind == GatewaySessionEventKind::dispatch) {
            // The events this handler routes itself, in list order.
            enum class Named : u32 {
                rate_limited,
                user_settings,
                voice_batch,
                voice_update,
                passive_update,
                supplemental,
                poll_votes,
                reactions,
                guild_create
            };
            const auto named = static_cast<Named>(protocol_detail::member_index<
                "RATE_LIMITED", "USER_SETTINGS_PROTO_UPDATE", "VOICE_STATE_UPDATE_BATCH", "VOICE_STATE_UPDATE",
                "PASSIVE_UPDATE_V2", "READY_SUPPLEMENTAL", "MESSAGE_POLL_VOTE_ADD_MANY",
                "MESSAGE_REACTION_ADD_MANY", "GUILD_CREATE">(event.envelope.event_name));
            // Neither grows while one dispatch is handled.
            const Slice<u32> index = main_index_view(runtime);
            const Slice<char> scratch = dispatch_scratch(runtime);
            RelationshipStreamBinding binding{client, runtime};
            if (named == Named::rate_limited) {
                GatewayRateLimited limited{};
                u32 rate_structurals[64]{};
                const auto decoded =
                    gateway_decode_rate_limited(event.envelope.data, {rate_structurals, 64}, 16, &limited);
                if (decoded.ok()) {
                    AcquireSRWLockExclusive(&runtime->subscription_lock);
                    gateway_member_request_rate_limited(&runtime->member_requests, limited, now_ms);
                    runtime->member_deadline_ms =
                        gateway_member_request_deadline(runtime->member_requests, now_ms);
                    gateway_channel_info_rate_limited(&runtime->channel_info, limited, now_ms);
                    runtime->channel_info_deadline_ms = gateway_channel_info_deadline(runtime->channel_info, now_ms);
                    ReleaseSRWLockExclusive(&runtime->subscription_lock);
                }
                checkpoint_resume_sequence(runtime);
                continue;
            }
            if (named == Named::user_settings) {
                self_settings_dispatch(client, runtime, event.envelope.data.source);
                settings_stores_dispatch(client, runtime, event.envelope, now_ms);
            }
            // STREAM_* carry the stream token: consumed here, never mailboxed.
            if (gateway_stream_dispatch(&runtime->stream.control, event.envelope, index, 64,
                                        now_ms)) {
                publish_voice_snapshot(client, runtime);
                checkpoint_resume_sequence(runtime);
                continue;
            }
            if (gateway_voice_dispatch(&runtime->voice, event.envelope, index, 64,
                                       now_ms)) {
                publish_voice_snapshot(client, runtime);
                const bool batch = named == Named::voice_batch;
                if ((batch || named == Named::voice_update) &&
                    !gateway_client_publish_voice_states(
                        event.envelope.data,
                        batch ? GatewayVoiceStateSource::batch : GatewayVoiceStateSource::update, 0,
                        index, scratch, event.envelope.sequence,
                        now_ms, publish_relationship_stream, &binding))
                    return resume_reconnect(client, runtime, now_ms);
                checkpoint_resume_sequence(runtime);
                continue;
            }
            if (named == Named::passive_update) {
                // Other members' voice joins/moves/leaves in passively synced
                // guilds; the rest of the document keeps its existing route.
                if (!gateway_client_publish_passive_voice_states(
                        event.envelope.data, index, scratch,
                        event.envelope.sequence, now_ms, publish_relationship_stream, &binding))
                    return resume_reconnect(client, runtime, now_ms);
            }
            GatewayClientEvent dispatch{};
            ConstSlice<u8> dispatch_payload{};
            u64 bulk_ids[kGatewayBulkDeleteMaximumIds]; // Only the ids the decoder counts are ever read.
            GatewayClientDispatchRouteResult routed{};
            const bool supplemental = named == Named::supplemental;
            const bool vote_batch = named == Named::poll_votes;
            const bool reaction_batch = vote_batch || named == Named::reactions;
            if (reaction_batch) {
                runtime->mailbox_deadline_ms = GetTickCount64() + kPhaseTimeoutMs;
                const auto batch =
                    vote_batch
                        ? gateway_client_publish_poll_vote_batch(event.envelope, index, 64,
                                                                 now_ms, publish_relationship_stream, &binding,
                                                                 &runtime->reaction_batch)
                        : gateway_client_publish_reaction_batch(
                              event.envelope, index, 64,
                              {reinterpret_cast<char*>(runtime->handshake_output + kMessageBytes),
                               kMessageReactionMaximumEmojiNameBytes},
                              now_ms, publish_relationship_stream, &binding, &runtime->reaction_batch);
                runtime->mailbox_deadline_ms = 0;
                if (batch.cancelled)
                    return resume_reconnect(client, runtime, now_ms);
                if (batch.ok()) {
                    checkpoint_resume_sequence(runtime);
                    continue;
                }
                set_decode_failure(&routed, batch.decoded);
            } else if (supplemental) {
                runtime->mailbox_deadline_ms = GetTickCount64() + kPhaseTimeoutMs;
                const Slice<u32> objects = object_scratch(runtime);
                JsonValue channels{};
                const auto decoded = gateway_decode_supplemental_private_channels(
                    event.envelope.data, index, 64, &channels);
                if (!decoded.ok()) {
                    set_decode_failure(&routed, decoded);
                } else if (channels.kind == JsonValueKind::array) {
                    const auto published =
                        publish_ready_state(client, runtime, channels, ReadyRecordKind::private_channel,
                                            event.envelope.sequence, now_ms);
                    routed = published.route;
                    if (published.publish_failed)
                        return resume_reconnect(client, runtime, now_ms);
                }
                if (routed.ok()) {
                    JsonValue merged{};
                    routed = gateway_relationship_merged_presences(event.envelope.data,
                                                                   index, &merged);
                    if (routed.ok()) {
                        routed = gateway_client_publish_friend_presences(
                            merged, index, objects,
                            event.envelope.sequence, now_ms, publish_relationship_stream, &binding);
                    }
                }
                // Server presences' rich activities; the member list carries their status.
                if (routed.ok()) {
                    routed = gateway_client_publish_guild_presences(
                        event.envelope.data, index, objects,
                        event.envelope.sequence, now_ms, publish_relationship_stream, &binding);
                }
                // Initial voice states of the other members arrive only here;
                // READY guilds carry none. Malformed guild rows are skipped.
                if (routed.ok()) {
                    if (!gateway_client_publish_supplemental_voice_states(
                            event.envelope.data, index, objects,
                            scratch, event.envelope.sequence, now_ms,
                            publish_relationship_stream, &binding))
                        return resume_reconnect(client, runtime, now_ms);
                }
                if (routed.ok()) {
                    if (!gateway_ready_publish_supplemental_members(
                             event.envelope.data, runtime->self_user_id, index,
                             objects, scratch, event.envelope.sequence,
                             now_ms, publish_relationship_stream, &binding)
                             .ok())
                        return resume_reconnect(client, runtime, now_ms);
                }
                if (routed.ok()) {
                    GatewayClientEvent complete{};
                    complete.kind = GatewayClientEventKind::dispatch;
                    complete.dispatch_kind = GatewayClientDispatchKind::supplemental_state_complete;
                    complete.timestamp_ms = now_ms;
                    complete.sequence = event.envelope.sequence;
                    complete.has_sequence = event.envelope.has_sequence;
                    if (!publish_event_payload(client, runtime, complete, {}))
                        return resume_reconnect(client, runtime, now_ms);
                    checkpoint_resume_sequence(runtime);
                    runtime->mailbox_deadline_ms = 0;
                    continue;
                }
                runtime->mailbox_deadline_ms = 0;
            } else
                routed = gateway_client_route_dispatch_large(
                    event.envelope, index, 64,
                    // The first half owns the decompressed envelope until routing
                    // completes. Message decoding revisits author/mention/rich
                    // JsonValue slices after copying content, so its output must
                    // use the disjoint second half. Aliasing these buffers
                    // corrupts the source in place and surfaces later as an
                    // unterminated string.
                    scratch, Slice<u64>{bulk_ids, kGatewayBulkDeleteMaximumIds},
                    runtime->self_user_id, {acquire_large_record, runtime}, &dispatch, &dispatch_payload,
                    &runtime->message_scratch);
            if (!routed.ok()) {
                settle_large_record(runtime);
                note_route_error(runtime, routed);
                // A schema/size failure in one evolving server dispatch must
                // not tear down an otherwise healthy Gateway. Queue a
                // pointer-free recovery marker; the UI catches up message
                // state through REST while the session continues at the next
                // sequence.
                gateway_client_route_failure_event(event.envelope, routed, now_ms, &dispatch);
                if (!publish_event_payload(client, runtime, dispatch, ConstSlice<u8>{}))
                    return resume_reconnect(client, runtime, now_ms);
                if (reaction_batch)
                    reset_value(&runtime->reaction_batch);
                // A joined guild that cannot be projected is still listed,
                // as unavailable, instead of silently missing.
                GatewayClientEvent placeholder{};
                if (named == Named::guild_create &&
                    gateway_client_route_guild_placeholder(event.envelope.data, index, 64,
                                                           &placeholder)) {
                    placeholder.timestamp_ms = now_ms;
                    placeholder.sequence = event.envelope.sequence;
                    placeholder.has_sequence = event.envelope.has_sequence;
                    if (!publish_event_payload(client, runtime, placeholder, ConstSlice<u8>{}))
                        return resume_reconnect(client, runtime, now_ms);
                }
                checkpoint_resume_sequence(runtime);
                continue;
            }
            if (dispatch.dispatch_kind == GatewayClientDispatchKind::account_required_action)
                InterlockedExchange(&client->required_action, static_cast<LONG>(dispatch.required_action));
            if (dispatch.dispatch_kind == GatewayClientDispatchKind::guild_members_chunk) {
                GatewayMemberChunkReceipt receipt{};
                if (gateway_decode_member_chunk_receipt(event.envelope.data, index, 64,
                                                        &receipt)
                        .ok()) {
                    AcquireSRWLockExclusive(&runtime->subscription_lock);
                    gateway_member_request_complete(&runtime->member_requests, receipt);
                    runtime->member_deadline_ms =
                        gateway_member_request_deadline(runtime->member_requests, now_ms);
                    ReleaseSRWLockExclusive(&runtime->subscription_lock);
                }
            }
            dispatch.timestamp_ms = now_ms;
            const bool dispatch_published =
                publish_event_payload(client, runtime, dispatch, dispatch_payload);
            settle_large_record(runtime);
            if (!dispatch_published)
                return resume_reconnect(client, runtime, now_ms);
            if (dispatch.dispatch_kind == GatewayClientDispatchKind::guild_create) {
                AcquireSRWLockExclusive(&runtime->subscription_lock);
                gateway_channel_info_forget(&runtime->channel_info, dispatch.state.id);
                ReleaseSRWLockExclusive(&runtime->subscription_lock);
                if (!gateway_client_publish_stage_instances(
                        event.envelope.data, dispatch.state.id, index,
                        scratch, event.envelope.sequence, now_ms,
                        publish_relationship_stream, &binding) ||
                    !gateway_client_publish_scheduled_events(
                        event.envelope.data, dispatch.state.id, index,
                        scratch, event.envelope.sequence, now_ms,
                        publish_relationship_stream, &binding) ||
                    !gateway_client_publish_voice_states(
                        event.envelope.data, GatewayVoiceStateSource::guild, dispatch.state.id,
                        index, scratch, event.envelope.sequence,
                        now_ms, publish_relationship_stream, &binding) ||
                    !gateway_client_publish_guild_stickers(
                        event.envelope.data, dispatch.state.id, index,
                        scratch, event.envelope.sequence, now_ms,
                        publish_relationship_stream, &binding))
                    return resume_reconnect(client, runtime, now_ms);
            }
            checkpoint_resume_sequence(runtime);
            continue;
        }
        if (event.kind == GatewaySessionEventKind::closed) {
            GatewayClientEvent closed{};
            closed.timestamp_ms = now_ms;
            closed.kind = GatewayClientEventKind::closed;
            closed.close_code = event.close_code;
            InterlockedExchange(&client->close_code, event.close_code);
            publish_event(runtime, closed);
            if (event.lifecycle_step.action == GatewayLifecycleAction::stop) {
                return fail_plaintext(runtime, GatewayClientFailureStage::close_stop);
            }
            u32 random = 0;
            if (!secure_random_u32(&random).ok()) {
                return fail_plaintext(runtime, GatewayClientFailureStage::close_random);
            }
            const GatewayReconnectDelay delay = gateway_reconnect_next(&runtime->reconnect_backoff, random);
            return schedule_reconnect(client, runtime, event.lifecycle_step.action, now_ms, delay.delay_ms)
                       ? PlaintextDrive::reconnecting
                       : fail_plaintext(runtime, GatewayClientFailureStage::close_reconnect);
        }
        if (event.kind == GatewaySessionEventKind::reconnect_requested) {
            return schedule_reconnect(client, runtime, event.lifecycle_step.action, now_ms, 0)
                       ? PlaintextDrive::reconnecting
                       : fail_plaintext(runtime, GatewayClientFailureStage::reconnect_request);
        }
        if (event.kind == GatewaySessionEventKind::invalid_session) {
            u32 random = 0;
            if (!secure_random_u32(&random).ok()) {
                return fail_plaintext(runtime, GatewayClientFailureStage::invalid_session_random);
            }
            return schedule_reconnect(client, runtime, event.lifecycle_step.action, now_ms,
                                      gateway_invalid_session_delay(random))
                       ? PlaintextDrive::reconnecting
                       : fail_plaintext(runtime, GatewayClientFailureStage::invalid_session_reconnect);
        }
        if (event.kind == GatewaySessionEventKind::error) {
            return fail_session_event(runtime, event);
        }
    }
}

PlaintextDrive process_encrypted_input(GatewayClient* client, GatewayClientRuntime* runtime,
                                       u64 now_ms) noexcept {
    while (byte_queue_size(runtime->encrypted_input) != 0) {
        const ConstSlice<u8> readable = byte_queue_readable(runtime->encrypted_input);
        const TlsDecryptResult decrypted =
            tls_client_decrypt(&runtime->tls, Slice<u8>{const_cast<u8*>(readable.data), readable.count});
        if (!decrypted.ok() || decrypted.state == TlsDecryptState::peer_closed ||
            decrypted.state == TlsDecryptState::renegotiate) {
            return fail_plaintext(runtime, GatewayClientFailureStage::tls_decrypt);
        }
        if (decrypted.state == TlsDecryptState::need_receive) {
            return PlaintextDrive::receive;
        }
        if (!gateway_session_push(&runtime->session,
                                  ConstSlice<u8>{decrypted.plaintext.data, decrypted.plaintext.count}) ||
            !byte_queue_consume(&runtime->encrypted_input, decrypted.consumed)) {
            return fail_plaintext(runtime, GatewayClientFailureStage::session_push);
        }
        const PlaintextDrive session = process_session(client, runtime, now_ms);
        if (session != PlaintextDrive::receive) {
            return session;
        }
    }
    return PlaintextDrive::receive;
}

PlaintextDrive resume_gateway_input(GatewayClient* client, GatewayClientRuntime* runtime,
                                    u64 now_ms) noexcept {
    const PlaintextDrive buffered = process_session(client, runtime, now_ms);
    return buffered == PlaintextDrive::receive ? process_encrypted_input(client, runtime, now_ms)
                                               : buffered;
}

// Sends what is queued; a failed send or receive restart ends the worker.
NetworkWorkerStep drive_output_step(GatewayClient* client, GatewayClientRuntime* runtime, u64 now_ms) noexcept {
    if (drive_application_output(client, runtime, now_ms) == OutboundDrive::failed)
        return fail_and_drain(client, runtime, GatewayClientError::session, 0);
    if (runtime->session.opened && !ensure_receive(runtime))
        return fail_and_drain(client, runtime, GatewayClientError::receive, WSAGetLastError());
    return NetworkWorkerStep{next_worker_wait(client, runtime, now_ms), false};
}

NetworkWorkerStep fail_send(GatewayClient* client, GatewayClientRuntime* runtime, u32 native_error,
                            u64 now_ms) noexcept {
    if (runtime->send_purpose == SendPurpose::guild_subscription)
        gateway_subscription_thread_owner_complete(&runtime->thread_subscription_owner, false);
    return reconnect_or_fail(client, runtime, GatewayClientError::send, native_error, now_ms);
}

NetworkWorkerStep handle_completion(GatewayClient* client, GatewayClientRuntime* runtime, NetworkLoop* loop,
                                    const NetworkWorkerEvent& event) noexcept {
    const NetworkCompletion& completion = event.poll.completion;
    if (voice_connection_owns(runtime->voice_connection, completion)) {
        voice_connection_completion(&runtime->voice_connection, runtime->voice.setup, loop, completion,
                                    event.now_ms);
        return {next_worker_wait(client, runtime, event.now_ms),
                runtime->stopping && shutdown_drained(runtime)};
    }
    if (gateway_stream_session_owns(runtime->stream, completion)) {
        gateway_stream_session_completion(&runtime->stream, loop, completion, event.now_ms);
        return {next_worker_wait(client, runtime, event.now_ms),
                runtime->stopping && shutdown_drained(runtime)};
    }
    // Capture wake-ups only need the voice drive that follows every event.
    if (completion.kind == NetworkOperationKind::signal && completion.signal == &runtime->voice_send_signal)
        return {next_worker_wait(client, runtime, event.now_ms), runtime->stopping && shutdown_drained(runtime)};
    if (((completion.kind == NetworkOperationKind::connect ||
          completion.kind == NetworkOperationKind::receive ||
          completion.kind == NetworkOperationKind::send) &&
         completion.connection != &runtime->connection) ||
        (completion.kind == NetworkOperationKind::resolve && completion.resolver != &runtime->resolver)) {
        return {next_worker_wait(client, runtime, event.now_ms), false};
    }
    if (completion.kind == NetworkOperationKind::signal &&
        (completion.signal == &runtime->subscription_signal ||
         completion.signal == &runtime->voice_signal)) {
        if (runtime->stopping) {
            return NetworkWorkerStep{INFINITE, shutdown_drained(runtime)};
        }
        AcquireSRWLockExclusive(&runtime->subscription_lock);
        gateway_state_recovery_schedule(&runtime->state_recovery, event.now_ms);
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        return drive_output_step(client, runtime, event.now_ms);
    }
    if (completion.kind == NetworkOperationKind::resolve && completion.resolver == &runtime->resolver) {
        if (runtime->stopping) {
            release_resolver(runtime);
            return NetworkWorkerStep{INFINITE, shutdown_drained(runtime)};
        }
        if (runtime->reconnecting) {
            release_resolver(runtime);
            return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
        }
        if (!completion.succeeded || !runtime->resolver.results) {
            return reconnect_or_fail(client, runtime, GatewayClientError::resolve,
                                     static_cast<u32>(completion.native_error), event.now_ms);
        }
        runtime->next_address = runtime->resolver.results;
        if (!start_connect_candidate(loop, runtime)) {
            return reconnect_or_fail(client, runtime, GatewayClientError::connect, WSAEHOSTUNREACH,
                                     event.now_ms);
        }
        publish_phase(client, GatewayClientPhase::connecting);
        runtime->deadline_ms = event.now_ms + kPhaseTimeoutMs;
        return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
    }

    if (completion.kind == NetworkOperationKind::connect ||
        completion.kind == NetworkOperationKind::receive || completion.kind == NetworkOperationKind::send) {
        if (runtime->outstanding_socket_operations != 0) {
            --runtime->outstanding_socket_operations;
        }
        if (runtime->stopping) {
            return NetworkWorkerStep{INFINITE, shutdown_drained(runtime)};
        }
        if (runtime->reconnecting) {
            return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
        }
    }

    if (completion.kind == NetworkOperationKind::connect) {
        if (!completion.succeeded || completion.stale) {
            network_connection_close(&runtime->connection);
            if (start_connect_candidate(loop, runtime)) {
                return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
            }
            return reconnect_or_fail(client, runtime, GatewayClientError::connect,
                                     static_cast<u32>(completion.native_error), event.now_ms);
        }
        release_resolver(runtime);
        const TlsResult initialized = tls_client_initialize(&runtime->tls, runtime->endpoint.host_wide);
        if (!initialized.ok() || runtime->tls.maximum_token_bytes > kHandshakeBytes) {
            return reconnect_or_fail(client, runtime, GatewayClientError::tls,
                                     static_cast<u32>(initialized.native_status), event.now_ms);
        }
        runtime->tls_initialized = true;
        publish_phase(client, GatewayClientPhase::tls_handshake);
        runtime->deadline_ms = event.now_ms + kPhaseTimeoutMs;
        if (!drive_tls_handshake(runtime)) {
            return reconnect_or_fail(client, runtime, GatewayClientError::tls, 0, event.now_ms);
        }
        return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
    }

    if (completion.kind == NetworkOperationKind::send) {
        if (!completion.succeeded || completion.stale || completion.transferred == 0)
            return fail_send(client, runtime, static_cast<u32>(completion.native_error), event.now_ms);
        runtime->send_offset += completion.transferred;
        if (runtime->send_offset < runtime->send_bytes) {
            if (!continue_send(runtime))
                return fail_send(client, runtime, WSAGetLastError(), event.now_ms);
            return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
        }
        const SendPurpose purpose = runtime->send_purpose;
        runtime->send_purpose = SendPurpose::none;
        if (purpose >= SendPurpose::gateway_action)
            SecureZeroMemory(runtime->record_output, runtime->send_bytes);
        if (purpose == SendPurpose::guild_subscription) {
            gateway_subscription_thread_owner_complete(&runtime->thread_subscription_owner, true);
            runtime->sent_subscription_revision = runtime->active_subscription_revision;
            runtime->active_subscription_revision = 0;
        }
        if (purpose == SendPurpose::self_status) {
            AcquireSRWLockExclusive(&runtime->subscription_lock);
            gateway_self_status_complete(&runtime->self_status, true);
            ++runtime->self_status.revision_serial;
            ReleaseSRWLockExclusive(&runtime->subscription_lock);
            if (client->notification_event)
                SetEvent(client->notification_event);
        }
        if (purpose == SendPurpose::tls_handshake) {
            if (runtime->after_tls_send == TlsHandshakeState::complete_and_send) {
                publish_phase(client, GatewayClientPhase::websocket_opening);
                runtime->deadline_ms = event.now_ms + kPhaseTimeoutMs;
                if (!begin_websocket_opening(runtime)) {
                    return reconnect_or_fail(client, runtime, GatewayClientError::websocket_opening, 0,
                                             event.now_ms);
                }
            } else if (!drive_tls_handshake(runtime)) {
                return reconnect_or_fail(client, runtime, GatewayClientError::tls, 0, event.now_ms);
            }
        } else if (purpose == SendPurpose::websocket_opening) {
            publish_phase(client, GatewayClientPhase::awaiting_hello);
            runtime->deadline_ms = event.now_ms + kPhaseTimeoutMs;
            if (!start_receive(runtime)) {
                return reconnect_or_fail(client, runtime, GatewayClientError::receive, WSAGetLastError(),
                                         event.now_ms);
            }
        } else if (purpose >= SendPurpose::pong) {
            const OutboundDrive outbound = drive_application_output(client, runtime, event.now_ms);
            if (outbound == OutboundDrive::failed) {
                return fail_and_drain(client, runtime, GatewayClientError::session, 0);
            }
            if (outbound != OutboundDrive::sending) {
                const PlaintextDrive driven = resume_gateway_input(client, runtime, event.now_ms);
                if (driven == PlaintextDrive::failed) {
                    return fail_and_drain(client, runtime, GatewayClientError::session, 0);
                }
                if (driven == PlaintextDrive::receive && !ensure_receive(runtime)) {
                    return fail_and_drain(client, runtime, GatewayClientError::receive, WSAGetLastError());
                }
            }
        }
        return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
    }

    if (completion.kind == NetworkOperationKind::receive) {
        if (!completion.succeeded || completion.stale || completion.transferred == 0 ||
            !byte_queue_produce(&runtime->encrypted_input, completion.transferred)) {
            return reconnect_or_fail(client, runtime, GatewayClientError::receive,
                                     static_cast<u32>(completion.native_error), event.now_ms);
        }
        if (!runtime->tls.handshake_complete) {
            if (!drive_tls_handshake(runtime)) {
                return reconnect_or_fail(client, runtime, GatewayClientError::tls, 0, event.now_ms);
            }
            return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
        }
        const PlaintextDrive driven = process_encrypted_input(client, runtime, event.now_ms);
        if (driven == PlaintextDrive::failed) {
            return fail_and_drain(client, runtime, GatewayClientError::session, 0);
        }
        if (driven == PlaintextDrive::receive && !ensure_receive(runtime)) {
            return fail_and_drain(client, runtime, GatewayClientError::receive, WSAGetLastError());
        }
        return NetworkWorkerStep{
            driven == PlaintextDrive::hello ? INFINITE : next_worker_wait(client, runtime, event.now_ms),
            false};
    }
    return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
}

NetworkWorkerStep gateway_worker_dispatch(void* opaque, NetworkLoop* loop,
                                          const NetworkWorkerEvent& event) noexcept {
    auto* client = static_cast<GatewayClient*>(opaque);
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    if (event.kind == NetworkWorkerEventKind::started) {
        runtime->clock = clock_initialize();
        runtime->start_counter = clock_now();
        runtime->connection.socket = INVALID_SOCKET;
        if (!begin_resolution(client, runtime, loop, event.now_ms)) {
            return fail_and_drain(client, runtime, GatewayClientError::resolve, WSAEINVAL);
        }
        return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
    }
    if (event.kind == NetworkWorkerEventKind::stop_requested) {
        begin_shutdown(client, runtime);
        return NetworkWorkerStep{INFINITE, shutdown_drained(runtime)};
    }
    if (event.kind == NetworkWorkerEventKind::deadline) {
        if (runtime->reconnecting) {
            if (runtime->resolver.pending || runtime->outstanding_socket_operations != 0) {
                return NetworkWorkerStep{INFINITE, false};
            }
            release_resolver(runtime);
            if (event.now_ms < runtime->reconnect_deadline_ms) {
                return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
            }
            if (!begin_resolution(client, runtime, loop, event.now_ms)) {
                return fail_and_drain(client, runtime, GatewayClientError::resolve, WSAEINVAL);
            }
            return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
        }
        if (gateway_voice_expire(&runtime->voice, event.now_ms))
            publish_voice_snapshot(client, runtime);
        AcquireSRWLockExclusive(&runtime->subscription_lock);
        const bool recover = gateway_state_recovery_due(
            &runtime->state_recovery, load_phase(client) == GatewayClientPhase::ready && !runtime->stopping,
            event.now_ms);
        ReleaseSRWLockExclusive(&runtime->subscription_lock);
        if (recover) {
            if (!schedule_reconnect(client, runtime, GatewayLifecycleAction::reconnect_identify,
                                    event.now_ms, 0))
                return fail_and_drain(client, runtime, GatewayClientError::session, 0);
            return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
        }
        if (!runtime->stopping && runtime->deadline_ms != 0 && event.now_ms >= runtime->deadline_ms &&
            load_phase(client) != GatewayClientPhase::hello) {
            return reconnect_or_fail(client, runtime, GatewayClientError::timeout, WAIT_TIMEOUT, event.now_ms);
        }
        if (runtime->authenticated && runtime->session.lifecycle.heartbeat_interval_ms != 0 &&
            event.now_ms >= runtime->session.lifecycle.next_heartbeat_ms) {
            const GatewayLifecycleStep heartbeat =
                gateway_lifecycle_tick(&runtime->session.lifecycle, event.now_ms);
            if (!heartbeat.ok()) {
                return fail_and_drain(client, runtime, GatewayClientError::session, 0);
            }
            if (heartbeat.action == GatewayLifecycleAction::send_heartbeat) {
                if (!gateway_outbound_enqueue(&runtime->outbound, heartbeat.action)) {
                    return fail_and_drain(client, runtime, GatewayClientError::session, 0);
                }
            } else if (heartbeat.action == GatewayLifecycleAction::reconnect_identify ||
                       heartbeat.action == GatewayLifecycleAction::reconnect_resume) {
                u32 random = 0;
                if (!secure_random_u32(&random).ok()) {
                    return fail_and_drain(client, runtime, GatewayClientError::random, 0);
                }
                const GatewayReconnectDelay delay =
                    gateway_reconnect_next(&runtime->reconnect_backoff, random);
                if (!schedule_reconnect(client, runtime, heartbeat.action, event.now_ms, delay.delay_ms)) {
                    return fail_and_drain(client, runtime, GatewayClientError::session, 0);
                }
                return NetworkWorkerStep{next_worker_wait(client, runtime, event.now_ms), false};
            }
        }
        if (runtime->rate_deadline_ms != 0 && event.now_ms >= runtime->rate_deadline_ms) {
            runtime->rate_deadline_ms = 0;
        }
        return drive_output_step(client, runtime, event.now_ms);
    }
    if (event.kind == NetworkWorkerEventKind::poll_error) {
        return fail_and_drain(client, runtime, GatewayClientError::worker,
                              static_cast<u32>(event.poll.completion.native_error));
    }
    return handle_completion(client, runtime, loop, event);
}

NetworkWorkerStep gateway_worker_handler(void* opaque, NetworkLoop* loop,
                                         const NetworkWorkerEvent& event) noexcept {
    auto* client = static_cast<GatewayClient*>(opaque);
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    const auto step = gateway_worker_dispatch(opaque, loop, event);
    // One consumer wake for everything this network event published.
    notify_consumer(client, runtime);
    if (step.exit)
        return step;
    // Main dispatch first: an endpoint/token replacement or leave in this
    // completion must invalidate the old voice attempt before further sends.
    const bool sending = !runtime->stopping && voice_send_active(runtime->voice_send);
    if (runtime->voice.requested || !voice_connection_drained(runtime->voice_connection) ||
        runtime->voice_connection.command.data || runtime->voice_receive.media_revision || sending) {
        runtime->voice_connection.video_decodable = voice_video_sink_decodable(runtime->voice_receive.video);
        voice_connection_drive(&runtime->voice_connection, runtime->voice.setup, loop, event.now_ms);
        apply_video_wants(client, runtime);
        voice_receive_drive(&runtime->voice_receive, &runtime->voice_connection, runtime->voice, event.now_ms);
        if (!runtime->stopping) {
            const auto& receive = runtime->voice_receive;
            voice_send_drive(&runtime->voice_send, &runtime->voice_connection, runtime->voice,
                             receive.media_revision && receive.media_revision == runtime->voice_connection.ready_revision,
                             loop, &runtime->voice_send_signal);
            voice_receive_configure(&runtime->voice_receive, runtime->voice_send.applied);
        }
        publish_voice_snapshot(client, runtime);
    }
    if (gateway_stream_session_busy(runtime->stream)) {
        gateway_stream_session_drive(&runtime->stream, runtime->voice, loop, runtime->voice_send.applied,
                                     event.now_ms);
        publish_voice_snapshot(client, runtime);
    }
    drive_client_activity(runtime, event.now_ms);
    notify_consumer(client, runtime);
    return {next_worker_wait(client, runtime, event.now_ms),
            runtime->stopping && shutdown_drained(runtime)};
}

// Frees the runtime of a start that failed after allocating it and records the error.
GatewayClientResult fail_start(GatewayClient* client, GatewayClientError error, u32 native_error) noexcept {
    release_runtime_allocation(client);
    client->runtime = nullptr;
    InterlockedExchange(&client->error, static_cast<LONG>(error));
    InterlockedExchange(&client->native_error, static_cast<LONG>(native_error));
    return result(client);
}

// Closes the events a failed start created so far; the ones not created yet are null.
void close_start_events(GatewayClient* client, GatewayClientRuntime* runtime) noexcept {
    if (runtime->mailbox_space)
        CloseHandle(runtime->mailbox_space);
    if (runtime->mailbox_cancel)
        CloseHandle(runtime->mailbox_cancel);
    if (client->notification_event)
        CloseHandle(client->notification_event);
    client->notification_event = nullptr;
}

} // namespace
namespace client_detail {

GatewayClientResult start_client(GatewayClient* client, SecureBuffer* credential,
                                 const AccountProfileMetadata* user_metadata,
                                 const GatewayUserClientConfiguration* user_configuration,
                                 const GatewayEndpoint* gateway_endpoint,
                                 const GatewayResumeState* resume_state, u64 resume_user_id) noexcept {
    const bool valid_credential =
        credential && credential->data && credential->locked && credential->bytes != 0 &&
        credential->bytes <= kGatewayUserMaximumTokenBytes &&
        utf8_validate(Utf8View{reinterpret_cast<const char*>(credential->data), credential->bytes}).valid;
    if (!client || client->worker.thread || client->runtime ||
        client->notification_event ||
        (resume_state && resume_state->valid && !gateway_resume_validate(*resume_state)) ||
        (gateway_endpoint && (!gateway_endpoint_validate(*gateway_endpoint) ||
                              gateway_endpoint->policy != GatewayEndpointPolicy::user_v9_etf_zstd_stream)) ||
        !valid_credential || !user_metadata || user_metadata->profile_id == 0 || !user_configuration ||
        !gateway_user_client_configuration_validate(*user_configuration)) {
        return invalid_input_result();
    }
    reset_value<GatewayClient>(client);
    const u32 allocation_bytes = kGatewayUserClientRuntimeBytes;
    client->runtime = VirtualAlloc(nullptr, allocation_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!client->runtime) {
        InterlockedExchange(&client->error, static_cast<LONG>(GatewayClientError::memory));
        InterlockedExchange(&client->native_error, static_cast<LONG>(thread_last_error()));
        return result(client);
    }
    auto* runtime = static_cast<GatewayClientRuntime*>(client->runtime);
    runtime->allocation_bytes = allocation_bytes;
    runtime->identity = GatewayClientIdentity::normal_user;
    runtime->owner = client;
    if (!stable_storage_initialize(&runtime->structural_scratch, kStructuralCount, kInitialStructuralCount,
                                   sizeof(u32), alignof(u32)) ||
        // Address space only: nothing is committed until an oversized guild.
        !reserved_bytes_initialize(&runtime->large_record, kGatewayClientGuildRecordCapacity) ||
        !stable_storage_initialize(&runtime->decoded_message, kGatewaySessionMaximumDecodedMessageBytes,
                                   kInitialDecodedMessageBytes, sizeof(u8), alignof(u8)) ||
        !stable_storage_initialize(&runtime->ready_record_index, kStructuralCount, kInitialStructuralCount,
                                   sizeof(u32), alignof(u32)) ||
        !stable_storage_initialize(&runtime->ready_arrays, kStructuralCount, kInitialStructuralCount,
                                   sizeof(u32), alignof(u32)) ||
        !reserved_bytes_initialize(&runtime->ready_staging, kReadyStagingBytes) ||
        !reserved_bytes_initialize(&runtime->compressed_spill, kGatewaySessionMaximumDecodedMessageBytes) ||
        !stable_storage_initialize(&runtime->zstd_storage, kZstdStorageBytes, kZstdInitialStorageBytes,
                                   sizeof(u8), alignof(u8))) {
        return fail_start(client, GatewayClientError::memory, thread_last_error());
    }
    runtime->user = reinterpret_cast<GatewayUserRuntime*>(static_cast<u8*>(client->runtime) +
                                                          kGatewayUserRuntimeOffset);
    runtime->user->metadata = *user_metadata;
    store_user_configuration(runtime->user, *user_configuration);
    gateway_self_status_initialize(&runtime->self_status, user_metadata->profile_id,
                                   {user_configuration->since_ms, user_configuration->status,
                                    user_configuration->has_since, user_configuration->afk});
    if (gateway_endpoint) {
        runtime->initial_endpoint = *gateway_endpoint;
    } else if (gateway_endpoint_parse_with_policy(
                   Utf8View{kGatewayUrl, static_cast<u32>(sizeof(kGatewayUrl) - 1)},
                   GatewayEndpointPolicy::user_v9_etf_zstd_stream, &runtime->initial_endpoint) !=
               GatewayEndpointError::none) {
        release_runtime_allocation(client);
        client->runtime = nullptr;
        return invalid_input_result();
    }
    if (resume_state && resume_state->valid) {
        runtime->resume = *resume_state;
        runtime->resume_sequence = static_cast<LONG64>(resume_state->sequence);
        runtime->self_user_id = runtime->resume_user_id = resume_user_id;
        gateway_self_status_bind(&runtime->self_status, resume_user_id);
    }
    const ConstSlice<u8> token_bytes = secure_buffer_view(*credential);
    char heartbeat_id[kGatewayUserLaunchIdBytes];
    const GatewayCommandWriteResult preflight = gateway_write_user_identify(
        user_identify(*runtime->user, Utf8View{reinterpret_cast<const char*>(token_bytes.data), token_bytes.count},
                      heartbeat_id),
        Slice<u8>{runtime->gateway_payload, sizeof(runtime->gateway_payload)});
    SecureZeroMemory(runtime->gateway_payload, sizeof(runtime->gateway_payload));
    if (!preflight.ok()) {
        release_runtime_allocation(client);
        client->runtime = nullptr;
        return invalid_input_result();
    }
    client->notification_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!client->notification_event) {
        const u32 native_error = thread_last_error();
        close_start_events(client, runtime);
        return fail_start(client, GatewayClientError::event, native_error);
    }
    runtime->mailbox_space = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    runtime->mailbox_cancel =
        runtime->mailbox_space ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
    if (!runtime->mailbox_space || !runtime->mailbox_cancel) {
        const u32 native_error = thread_last_error();
        close_start_events(client, runtime);
        return fail_start(client, GatewayClientError::event, native_error);
    }
    if (!secure_buffer_transfer(&runtime->credential, credential)) {
        close_start_events(client, runtime);
        release_runtime_allocation(client);
        reset_value<GatewayClient>(client);
        return invalid_input_result();
    }
    runtime->authenticated = true;
    const NetworkWorkerResult started =
        network_worker_start(&client->worker, gateway_worker_handler, client);
    if (!started.ok()) {
        if (runtime->credential.data)
            secure_buffer_transfer(credential, &runtime->credential);
        close_start_events(client, runtime);
        return fail_start(client, GatewayClientError::worker, started.native_error);
    }
    return result(client);
}

} // namespace client_detail
} // namespace maicord
