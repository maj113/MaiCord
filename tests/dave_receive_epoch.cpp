#include "maicord/app/dave_receive_epoch.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/dave_epoch_vectors.hpp"
u64 age(u32 generation) noexcept {
    return ((u64{generation} * (u64{1} << 24) + 169) / 170) * 1000;
}
bool initialize(Check check, DaveReceiveEpoch& epoch, Gcm64Backend backend) noexcept {
    u8 seed[16];
    for (u32 i = 0; i != 16; ++i)
        seed[i] = static_cast<u8>(i);
    const auto result = dave_receive_epoch_initialize(&epoch, {seed, 16}, 0, backend);
    check(result.ok(), "DAVE sender epoch initializes protected seed");
    return result.ok();
}
struct Frame {
    u8 wire[64], output[48], scratch[32];
    DaveClearRange ranges[1];
    DaveFrameView view;
};
bool prepare(Check check, Frame& frame, u32 generation, u32 counter) noexcept {
    for (const auto& vector : epoch_vectors) {
        if (vector.generation != generation || vector.counter != counter)
            continue;
        const u32 size = test_unhex(vector.wire, frame.wire);
        memset(frame.output, 0xa5, sizeof(frame.output));
        memset(frame.scratch, 0xa5, sizeof(frame.scratch));
        const auto parsed = dave_frame_parse({frame.wire, size}, {frame.ranges, 1}, &frame.view);
        check(parsed.ok(), "DAVE epoch fixture parses");
        return parsed.ok();
    }
    check(false, "DAVE epoch fixture exists");
    return false;
}
DaveEpochOpenResult open(DaveReceiveEpoch& epoch, KdfSha256Workspace& workspace, Frame& frame,
                         u64 now) noexcept {
    return dave_receive_epoch_open(epoch, workspace, frame.view, now, {frame.scratch, 32},
                                   {frame.output, 48});
}
void correct(Check check, const DaveEpochOpenResult& result, const Frame& frame) noexcept {
    bool matched = result.ok() && result.frame.ok() && result.frame.bytes == 32;
    for (u32 i = 0; i != 32 && matched; ++i)
        matched = frame.output[i] == static_cast<u8>(i * 17 + 9);
    check(matched && test_filled(frame.output + 32, 16, 0xa5),
          "DAVE sender key owner opens independent wire fixture and preserves output tail");
}
void lifecycle(Check check, KdfSha256Workspace& workspace, Gcm64Backend backend) noexcept {
    DaveReceiveEpoch epoch{};
    Frame frame{};
    if (!initialize(check, epoch, backend))
        return;
    prepare(check, frame, 1, 1);
    check(open(epoch, workspace, frame, age(1) - 1).error == DaveEpochError::generation_lifetime &&
              epoch.next_generation == 0 && !epoch.replay.initialized,
          "DAVE lifetime guard rejects future generation before deriving any key");
    prepare(check, frame, 0, 1);
    correct(check, open(epoch, workspace, frame, age(1)), frame);
    const auto* storage = epoch.storage.data;
    const auto duplicate = open(epoch, workspace, frame, age(1));
    check(duplicate.error == DaveEpochError::frame &&
              duplicate.frame.error == DaveOpenError::replay_rejected && !duplicate.frame.crypto_attempted,
          "DAVE epoch rejects duplicate before cryptography");
    prepare(check, frame, 0, 2);
    correct(check, open(epoch, workspace, frame, age(1)), frame);
    check(epoch.storage.data == storage && epoch.next_generation == 1 && epoch.count == 1,
          "DAVE cached generation reuses allocation and derivation");
    prepare(check, frame, 1, 1);
    frame.wire[0] ^= 1;
    const auto forged = open(epoch, workspace, frame, age(1));
    check(forged.error == DaveEpochError::frame &&
              forged.frame.error == DaveOpenError::authentication_failed && epoch.next_generation == 2 &&
              epoch.newest_generation == 0 && epoch.replay.largest == 2 &&
              epoch.next_key_expiry_ms == ~u64{0},
          "Forged future key can be cached but cannot advance replay or authenticated generation/expiry");
    frame.wire[0] ^= 1;
    correct(check, open(epoch, workspace, frame, age(1)), frame);
    check(epoch.newest_generation == 1 && epoch.replay.largest == (u64{1} << 24) + 1 &&
              epoch.next_key_expiry_ms == age(1) + 10000,
          "Authentic new generation advances replay and starts old-key expiry");
    prepare(check, frame, 1, 2);
    check(open(epoch, workspace, frame, age(1) - 1).error == DaveEpochError::invalid_time &&
              test_filled(frame.output, 48, 0xa5),
          "DAVE rejects clock rollback without writes");
    epoch.expiry_ms = age(1) + 1;
    correct(check, open(epoch, workspace, frame, age(1) + 1), frame);
    check(open(epoch, workspace, frame, age(1) + 2).error == DaveEpochError::expired,
          "DAVE epoch expiry is strict and checked before replay/crypto");
    dave_receive_epoch_release(&epoch);
    check(!epoch.storage.data && !epoch.count && !epoch.replay.initialized && !epoch.next_generation,
          "DAVE epoch release retires every retained key and replay state");
    dave_receive_epoch_release(&epoch);
}
void reordering(Check check, KdfSha256Workspace& workspace, Gcm64Backend backend) noexcept {
    DaveReceiveEpoch epoch{};
    Frame frame{};
    if (!initialize(check, epoch, backend))
        return;
    prepare(check, frame, 0, 0xfffffd);
    correct(check, open(epoch, workspace, frame, age(1)), frame);
    prepare(check, frame, 1, 2);
    correct(check, open(epoch, workspace, frame, age(1)), frame);
    prepare(check, frame, 0, 0xfffffe);
    correct(check, open(epoch, workspace, frame, age(1) + 10000), frame);
    check(epoch.oldest_generation == 0 && epoch.newest_generation == 1,
          "DAVE retains old key for reordered authenticated frame through exact expiry boundary");
    prepare(check, frame, 0, 0xffffff);
    check(open(epoch, workspace, frame, age(1) + 10001).error == DaveEpochError::generation_gap &&
              epoch.oldest_generation == 1 && epoch.count == 1 && test_filled(frame.output, 48, 0xa5),
          "DAVE expired key is removed and cannot authenticate even a missing nonce");
    prepare(check, frame, 1, 1);
    correct(check, open(epoch, workspace, frame, age(1) + 10001), frame);
    dave_receive_epoch_release(&epoch);
}
void gaps_and_wrap(Check check, KdfSha256Workspace& workspace, Gcm64Backend backend) noexcept {
    DaveReceiveEpoch epoch{};
    Frame frame{};
    if (!initialize(check, epoch, backend))
        return;
    prepare(check, frame, 251, 1);
    check(open(epoch, workspace, frame, age(257)).error == DaveEpochError::generation_gap &&
              epoch.next_generation == 0,
          "DAVE rejects generation gap 251 before deriving keys");
    constexpr u32 generations[]{250, 251, 255, 256, 257};
    for (u32 generation : generations) {
        prepare(check, frame, generation, 1);
        correct(check, open(epoch, workspace, frame, age(257) + u64{generation} * 11000), frame);
        check(epoch.next_generation == u64{generation} + 1 && epoch.newest_generation == generation &&
                  epoch.replay.largest == (u64{generation} << 24) + 1,
              "DAVE sequential secrets survive growth, compaction and eight-bit generation wrap");
    }
    dave_receive_epoch_release(&epoch);
    if (!initialize(check, epoch, backend))
        return;
    // Synthetic checkpoint avoids billions of derivations while exercising the
    // real final derivation and wire generation/nonce guards.
    epoch.oldest_generation = epoch.newest_generation = ~u32{0};
    epoch.next_generation = ~u32{0};
    prepare(check, frame, ~u32{0}, 1);
    correct(check, open(epoch, workspace, frame, age(~u32{0})), frame);
    check(epoch.next_generation == (u64{1} << 32),
          "DAVE next generation retains exhausted uint32 terminal value");
    prepare(check, frame, ~u32{0}, 2);
    correct(check, open(epoch, workspace, frame, age(~u32{0})), frame);
    prepare(check, frame, 0, 1);
    check(open(epoch, workspace, frame, age(~u32{0})).error == DaveEpochError::generation_gap &&
              epoch.next_generation == (u64{1} << 32),
          "DAVE refuses to wrap exhausted key generation to zero");
    dave_receive_epoch_release(&epoch);
}
void growth_and_retry(Check check, KdfSha256Workspace& workspace, Gcm64Backend backend) noexcept {
    DaveReceiveEpoch epoch{};
    Frame frame{};
    if (!initialize(check, epoch, backend))
        return;
    prepare(check, frame, 0, 1);
    correct(check, open(epoch, workspace, frame, age(250)), frame);
    const u32 initial_capacity = epoch.capacity;
    prepare(check, frame, 5, 1);
    frame.wire[0] ^= 1;
    check(open(epoch, workspace, frame, age(250)).frame.error == DaveOpenError::authentication_failed &&
              epoch.capacity > initial_capacity && epoch.next_generation == 6 && !epoch.newest_generation,
          "DAVE growth transfers existing CNG key ownership while retaining skipped generation keys");
    prepare(check, frame, 0, 2);
    correct(check, open(epoch, workspace, frame, age(250)), frame);
    prepare(check, frame, 250, 1);
    frame.wire[0] ^= 1;
    check(open(epoch, workspace, frame, age(250)).frame.error == DaveOpenError::authentication_failed &&
              epoch.count == 251 && epoch.next_generation == 251 && epoch.replay.largest == 2,
          "DAVE larger growth preserves all retained keys without authenticating forged future frame");
    prepare(check, frame, 2, 1);
    correct(check, open(epoch, workspace, frame, age(250)), frame);
    check(epoch.next_generation == 251 && epoch.newest_generation == 2,
          "DAVE imports a skipped key without re-deriving or advancing the next secret");
    dave_receive_epoch_release(&epoch);

    DaveReceiveEpoch wrong{};
    u8 wrong_seed[16]{};
    check(dave_receive_epoch_initialize(&wrong, {wrong_seed, 16}, 0, backend).ok(),
          "DAVE alternate epoch initialization");
    if (!initialize(check, epoch, backend)) {
        dave_receive_epoch_release(&wrong);
        return;
    }
    prepare(check, frame, 0, 1);
    u8 original[64];
    copy_bytes(original, frame.wire, frame.view.frame.count);
    const auto rejected = dave_receive_epoch_open(wrong, workspace, frame.view, 0, {frame.scratch, 32},
                                                  {frame.wire, frame.view.frame.count});
    check(rejected.frame.error == DaveOpenError::authentication_failed && !wrong.replay.initialized &&
              bytes_equal(frame.wire, original, frame.view.frame.count) &&
              test_filled(frame.scratch, 32, 0),
          "DAVE wrong epoch preserves in-place ciphertext and does not consume nonce");
    const auto accepted = dave_receive_epoch_open(epoch, workspace, frame.view, 0, {frame.scratch, 32},
                                                  {frame.wire, frame.view.frame.count});
    bool plaintext = accepted.ok() && accepted.frame.bytes == 32;
    for (u32 i = 0; i != 32 && plaintext; ++i)
        plaintext = frame.wire[i] == static_cast<u8>(i * 17 + 9);
    check(plaintext && bytes_equal(frame.wire + 32, original + 32, frame.view.frame.count - 32) &&
              !wrong.replay.initialized && epoch.replay.initialized,
          "DAVE alternate epoch successfully retries the same borrowed in-place frame");
    dave_receive_epoch_release(&wrong);
    dave_receive_epoch_release(&epoch);
}
void failures(Check check, KdfSha256Workspace& workspace) noexcept {
    DaveReceiveEpoch epoch{};
    Frame frame{};
    if (!initialize(check, epoch, Gcm64Backend::automatic))
        return;
    u8 seed[16]{};
    check(dave_receive_epoch_initialize(&epoch, {seed, 16}, 0).error == KdfError::invalid_input,
          "DAVE cannot overwrite live sender epoch");
    prepare(check, frame, 0, 1);
    const u32 capacity = workspace.storage.capacity;
    for (u32 size = 0; size != 32; ++size) {
        const auto short_output =
            dave_receive_epoch_open(epoch, workspace, frame.view, 0, {}, {frame.output, size});
        const auto short_scratch = dave_receive_epoch_open(epoch, workspace, frame.view, 0,
                                                           {frame.scratch, size}, {frame.wire, 64});
        check(short_output.frame.error == DaveOpenError::output_capacity &&
                  short_scratch.frame.error == DaveOpenError::scratch_capacity && !epoch.count &&
                  !epoch.next_generation && test_filled(frame.output, 48, 0xa5),
              "DAVE short caller buffers fail before key derivation/import or writes");
    }
    workspace.storage.capacity = 1;
    const auto failed = open(epoch, workspace, frame, 0);
    workspace.storage.capacity = capacity;
    check(failed.error == DaveEpochError::derivation &&
              failed.derivation.error == KdfError::platform_failure && !epoch.count &&
              !epoch.next_generation && !epoch.replay.initialized && test_filled(frame.output, 48, 0xa5),
          "DAVE provider failure cannot skip generation or publish plaintext");
    correct(check, open(epoch, workspace, frame, 0), frame);
    prepare(check, frame, 0, 2);
    check(dave_receive_epoch_open(epoch, workspace, frame.view, 0, {}, {epoch.storage.data, 32}).error ==
              DaveEpochError::invalid_input,
          "DAVE protects retained secrets from caller output alias");
    check(dave_receive_epoch_open(epoch, workspace, frame.view, 0, {}, {reinterpret_cast<u8*>(&epoch), 32})
                  .error == DaveEpochError::invalid_input,
          "DAVE protects complete epoch owner from caller output alias");
    check(
        dave_receive_epoch_open(epoch, workspace, frame.view, 0, {}, {workspace.storage.data, 32}).error ==
            DaveEpochError::invalid_input,
        "DAVE protects shared worker KDF workspace from frame output");
    correct(check, open(epoch, workspace, frame, 0), frame);
    dave_receive_epoch_release(&epoch);
}
} // namespace
void test_dave_receive_epoch(Check check) noexcept {
    KdfSha256Workspace workspace{};
    check(kdf_sha256_initialize(&workspace).ok(), "DAVE epoch test KDF workspace");
    if (!workspace.storage.data)
        return;
    constexpr Gcm64Backend backends[]{Gcm64Backend::automatic, Gcm64Backend::portable};
    for (auto backend : backends) {
        lifecycle(check, workspace, backend);
        reordering(check, workspace, backend);
        gaps_and_wrap(check, workspace, backend);
        growth_and_retry(check, workspace, backend);
    }
    failures(check, workspace);
    kdf_sha256_release(&workspace);
}
} // namespace maicord
