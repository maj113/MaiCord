#include "maicord/protocol/dave_replay.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
static_assert(sizeof(DaveReplay) <= 8192);
// Independent ordered-list model of libdave's missing-nonce deque policy.
// Deliberately uses linear lookup/front erasure, not the production ring/index.
struct Model {
    u64 largest, missing[1000];
    u32 count;
    bool initialized;
};
bool permits(const Model& model, u64 value) noexcept {
    if (value > kDaveMaximumFrameCounter)
        return false;
    if (!model.initialized || value > model.largest)
        return true;
    for (u32 i = 0; i != model.count; ++i)
        if (model.missing[i] == value)
            return true;
    return false;
}
void consume(Model& model, u64 value) noexcept {
    if (!model.initialized) {
        model.largest = value;
        model.initialized = true;
        return;
    }
    if (value > model.largest) {
        const auto missing = minimum<u64>(value - model.largest - 1, 1000);
        const auto drop = model.count + missing > 1000 ? model.count + missing - 1000 : 0;
        model.count -= static_cast<u32>(drop);
        for (u32 i = 0; drop && i != model.count; ++i)
            model.missing[i] = model.missing[i + drop];
        for (u64 next = value - missing; next != value; ++next)
            model.missing[model.count++] = next;
        model.largest = value;
    } else {
        u32 at = 0;
        while (at < model.count && model.missing[at] != value)
            ++at;
        if (at != model.count) {
            --model.count;
            for (; at != model.count; ++at)
                model.missing[at] = model.missing[at + 1];
        }
    }
}
void boundaries(Check check) noexcept {
    struct CounterCase {
        u32 oldest, truncated, generation;
        u64 value;
        bool valid;
    };
    const CounterCase cases[]{{0, 0, 0, 0, true},
                              {0, 0xFFFFFF, 0, 0xFFFFFF, true},
                              {0, 0x01000000, 1, 0x1000000, true},
                              {254, 0xFFFFFFFF, 255, 0xFFFFFFFF, true},
                              {255, 0, 256, 0x100000000, true},
                              {259, 0x03000001, 259, (u64{259} << 24) | 1, true},
                              {259, 0x02000001, 514, (u64{514} << 24) | 1, true},
                              {~u32{0}, ~u32{0}, ~u32{0}, kDaveMaximumFrameCounter, true},
                              {~u32{0}, 0, 0, 0, false}};
    for (const auto& c : cases) {
        DaveFrameCounter value{123, 456};
        check(dave_frame_counter(c.oldest, c.truncated, &value) == c.valid &&
                  (c.valid ? value.value == c.value && value.generation == c.generation
                           : value.value == 123 && value.generation == 456),
              "DAVE generation expansion preserves wrap/overflow and output boundaries");
    }
    check(!dave_frame_counter(0, 0, nullptr), "DAVE counter expansion requires output");
    DaveReplay replay{};
    check(dave_replay_can_accept(replay, 0) && dave_replay_commit(&replay, 0) &&
              !dave_replay_can_accept(replay, 0) && !dave_replay_commit(&replay, 0),
          "DAVE initial zero nonce is consumed exactly once");
    check(dave_replay_commit(&replay, 2) && dave_replay_can_accept(replay, 1),
          "DAVE creates an intervening gap");
    for (u64 counter = 3; counter != 100003; ++counter)
        if (!dave_replay_commit(&replay, counter)) {
            check(false, "DAVE contiguous advance");
            break;
        }
    check(replay.count == 1 && dave_replay_can_accept(replay, 1) && dave_replay_commit(&replay, 1) &&
              !replay.count && !dave_replay_can_accept(replay, 1),
          "DAVE retains old missing nonce through contiguous advance, unlike a sliding window");
    clear_bytes(&replay, sizeof(replay));
    check(dave_replay_commit(&replay, 50) && !dave_replay_can_accept(replay, 49) &&
              dave_replay_commit(&replay, 5000) && replay.count == 1000 &&
              !dave_replay_can_accept(replay, 3999) && dave_replay_can_accept(replay, 4000) &&
              dave_replay_can_accept(replay, 4999),
          "DAVE initial nonce creates no earlier gaps and a large jump retains exactly the latest 1000 "
          "holes");
    check(dave_replay_commit(&replay, 4500) && dave_replay_commit(&replay, 4000) &&
              dave_replay_commit(&replay, 4999) && !dave_replay_can_accept(replay, 4500) &&
              dave_replay_can_accept(replay, 4499) && dave_replay_can_accept(replay, 4501),
          "DAVE consumes middle/front/back gaps without dropping adjacent missing nonces");
    check(dave_replay_can_accept(replay, 900000) && replay.largest == 5000 &&
              dave_replay_can_accept(replay, 5001),
          "DAVE preflight cannot advance state before successful authentication");
    DaveReplay before = replay;
    check(!dave_replay_commit(&replay, kDaveMaximumFrameCounter + 1) &&
              bytes_equal(&before, &replay, sizeof(replay)),
          "DAVE impossible counter rejection leaves all replay state unchanged");
    check(dave_replay_commit(&replay, kDaveMaximumFrameCounter) &&
              !dave_replay_can_accept(replay, kDaveMaximumFrameCounter) &&
              dave_replay_can_accept(replay, kDaveMaximumFrameCounter - 1000) &&
              !dave_replay_can_accept(replay, kDaveMaximumFrameCounter - 1001),
          "DAVE maximum wide counter does not wrap");
}
void reference(Check check) noexcept {
    DaveReplay replay{};
    Model model{};
    u32 random = 0xA61D34B7;
    for (u32 step = 0; step != 20000; ++step) {
        random = random * 1664525u + 1013904223u;
        u64 candidate;
        switch (step % 7) {
        case 0:
            candidate = model.largest + 1;
            break;
        case 1:
            candidate = model.largest + (random % 1700) + 1;
            break;
        case 2:
            candidate = model.count ? model.missing[random % model.count] : model.largest;
            break;
        case 3:
            candidate = model.largest;
            break;
        case 4:
            candidate = random % (model.largest + 1);
            break;
        case 5:
            candidate = model.largest + 100000;
            break;
        default:
            candidate = model.count ? model.missing[model.count / 2] : 0;
            break;
        }
        const bool allowed = permits(model, candidate);
        check(dave_replay_can_accept(replay, candidate) == allowed,
              "DAVE replay preflight agrees with independent deque-policy model");
        // Simulate authentication failure without reporting success.
        if (step % 19 != 0 && step % 7 != 5) {
            check(dave_replay_commit(&replay, candidate) == allowed,
                  "DAVE replay commit agrees with independent model");
            if (allowed)
                consume(model, candidate);
        }
        bool equal = replay.count == model.count && replay.largest == model.largest &&
                     replay.initialized == model.initialized;
        for (u32 i = 0; equal && i != model.count; ++i)
            equal = replay.missing[(replay.head + i) % kDaveReplaySlots] == model.missing[i];
        check(equal, "DAVE ring preserves ordered missing-nonce policy through wrap, erasure, loss and "
                     "failed authentication");
        if (!equal)
            break;
    }
}
} // namespace
void test_dave_replay(Check check) noexcept {
    boundaries(check);
    reference(check);
}
} // namespace maicord
