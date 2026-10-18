#include "maicord/app/dave_session.hpp"
#include "maicord/app/mls_exporter.hpp"
#include "maicord/app/voice_connection.hpp"
#include "maicord/core/memory.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include "test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
#include "data/dave_epoch_vectors.hpp"
#include "data/mls_tree_math_vectors.hpp"

constexpr u32 kNone = 0xffffffffu;

// ExternalSender as the voice gateway serializes it (RFC 9420 varint vectors):
// opaque signature_key<V> (65 bytes: two-byte length 0x4041), basic credential
// type 1, opaque identity<V> (under 64 bytes: one-byte length).
MlsStatus write_external_sender(const u8* signature_key, ConstSlice<u8> identity, Slice<u8> output,
                                u32* written) noexcept {
    const u32 bytes = 2 + 65 + 2 + 1 + identity.count;
    if (!signature_key || signature_key[0] != 4 || !identity.count || identity.count > 63 || !written ||
        output.count < bytes)
        return MlsStatus::invalid_argument;
    output[0] = 0x40;
    output[1] = 65;
    copy_bytes(output.data + 2, signature_key, 65);
    output[67] = 0;
    output[68] = 1;
    output[69] = static_cast<u8>(identity.count);
    copy_bytes(output.data + 70, identity.data, identity.count);
    *written = bytes;
    return MlsStatus::ok;
}

void tree_math(Check check) noexcept {
    bool relations = true;
    for (const auto& v : tree_math_vectors) {
        u32 root = 0, parent = 0, sibling = 0;
        relations = relations && mls_tree_root(v.leaves, &root) == MlsStatus::ok && root == v.root;
        const bool leaf = mls_tree_is_leaf(v.node);
        relations = relations && (leaf ? v.left == kNone : mls_tree_left(v.node) == v.left) &&
                    (leaf ? v.right == kNone : mls_tree_right(v.node) == v.right);
        if (v.parent == kNone)
            relations = relations && v.node == v.root;
        else
            relations = relations && mls_tree_parent(v.node, &parent) == MlsStatus::ok &&
                        parent == v.parent && mls_tree_sibling(v.node, parent, &sibling) == MlsStatus::ok &&
                        sibling == v.sibling;
    }
    check(relations, "MLS tree root/left/right/parent/sibling match the RFC 9420 appendix C oracle");
    bool paths = true;
    for (const auto& v : tree_path_vectors) {
        u32 direct[kMlsTreeMaxDepth], copath[kMlsTreeMaxDepth], direct_count = 0, copath_count = 0;
        paths =
            paths &&
            mls_tree_direct_path(v.node, v.leaves, {direct, kMlsTreeMaxDepth}, &direct_count) ==
                MlsStatus::ok &&
            mls_tree_copath(v.node, v.leaves, {copath, kMlsTreeMaxDepth}, &copath_count) == MlsStatus::ok &&
            direct_count == v.count && copath_count == v.count;
        for (u32 i = 0; paths && i != v.count; ++i)
            paths = direct[i] == v.direct[i] && copath[i] == v.copath[i];
    }
    u32 small[1], count = 0;
    check(paths && mls_tree_direct_path(0, 4, {small, 1}, &count) == MlsStatus::buffer_too_small &&
              mls_tree_direct_path(7, 4, {small, 1}, &count) == MlsStatus::invalid_argument,
          "MLS direct paths and copaths match the oracle and reject short or out-of-range requests");
    bool ancestors = true;
    for (const auto& v : tree_ancestor_vectors) {
        u32 ancestor = 0;
        ancestors = ancestors &&
                    mls_tree_leaf_ancestor(v.left_leaf, v.right_leaf, &ancestor) == MlsStatus::ok &&
                    ancestor == v.ancestor;
    }
    check(ancestors, "MLS lowest common ancestors of leaf pairs match the oracle");
}

// The outbound sealer must reproduce the independent receive-epoch oracle
// byte for byte: same ratchet, nonce, tag truncation and frame suffix.
void sealer(Check check, KdfSha256Workspace& kdf) noexcept {
    u8 seed[16], body[32], wire[96], expected[96], output[96];
    for (u32 i = 0; i != 16; ++i)
        seed[i] = static_cast<u8>(i);
    for (u32 i = 0; i != 32; ++i)
        body[i] = static_cast<u8>((i * 17 + 9) & 255);
    bool matched = true;
    u32 checked = 0;
    for (const auto& vector : epoch_vectors) {
        if (vector.generation > 255)
            continue;
        DaveFrameSealer frames{};
        matched = matched && dave_frame_sealer_initialize(&frames, {seed, 16}).ok();
        frames.truncated_nonce = (vector.generation << 24) + vector.counter - 1;
        const u32 size = test_unhex(vector.wire, expected);
        const auto sealed = dave_frame_seal(frames, kdf, {body, 32}, {output, sizeof(output)});
        matched = matched && sealed.ok() && sealed.bytes == size && bytes_equal(output, expected, size) &&
                  sealed.generation == vector.generation;
        dave_frame_sealer_release(&frames);
        ++checked;
    }
    check(matched && checked >= 10, "DAVE sealer reproduces oracle frames across ratchet generations");
    DaveFrameSealer frames{};
    dave_frame_sealer_initialize(&frames, {seed, 16});
    copy_bytes(wire, body, 32);
    const auto first = dave_frame_seal(frames, kdf, {wire, 32}, {wire, sizeof(wire)});
    test_unhex(epoch_vectors[0].wire, expected);
    const auto small = dave_frame_seal(frames, kdf, {body, 32}, {output, 32 + kDaveSealOverhead - 6});
    const auto second = dave_frame_seal(frames, kdf, {body, 32}, {output, sizeof(output)});
    test_unhex(epoch_vectors[1].wire, expected);
    check(first.ok() && first.nonce == 1 && small.error == DaveSealError::output_capacity && second.ok() &&
              second.nonce == 2 && bytes_equal(output, expected, second.bytes),
          "DAVE sealer works in place and a capacity failure does not consume a nonce");
    frames.truncated_nonce = ~u32{0};
    check(dave_frame_seal(frames, kdf, {body, 32}, {output, sizeof(output)}).error ==
              DaveSealError::exhausted,
          "DAVE sealer refuses to wrap its 32-bit nonce");
    dave_frame_sealer_release(&frames);
}

// Minimal test-side MLS writer for the gateway's external proposals.
struct Out {
    u8* data;
    u32 used;
    void bytes(const void* value, u32 count) noexcept {
        copy_bytes(data + used, value, count);
        used += count;
    }
    void integer(u64 value, u32 count) noexcept {
        for (u32 i = 0; i != count; ++i)
            data[used++] = static_cast<u8>(value >> (8 * (count - 1 - i)));
    }
    void varint(u32 value) noexcept {
        if (value < 64)
            integer(value, 1);
        else if (value < 16384)
            integer(0x4000 | value, 2);
        else
            integer(0x80000000u | value, 4);
    }
    void vector(const void* value, u32 count) noexcept {
        varint(count);
        bytes(value, count);
    }
};
// External-sender PublicMessage carrying one Add or Remove, signed by the
// test gateway over FramedContentTBS (no GroupContext for external senders).
u32 proposal(KdfSha256Workspace& kdf, const P256Key& gateway, u64 group_id, u64 epoch,
             ConstSlice<u8> package, u32 removed, u8* output) noexcept {
    Out out{output, 0};
    u8 id[8], signature[72];
    for (u32 i = 0; i != 8; ++i)
        id[i] = static_cast<u8>(group_id >> (56 - 8 * i));
    out.integer(1, 2);
    out.integer(1, 2);
    out.vector(id, 8);
    out.integer(epoch, 8);
    out.integer(2, 1);
    out.integer(0, 4);
    out.varint(0);
    out.integer(2, 1);
    if (package.count) {
        out.integer(kMlsProposalAdd, 2);
        out.bytes(package.data, package.count);
    } else {
        out.integer(kMlsProposalRemove, 2);
        out.integer(removed, 4);
    }
    const auto signed_result =
        mls_sign_with_label(kdf, gateway, {reinterpret_cast<const u8*>("FramedContentTBS"), 16},
                            {output, out.used}, {}, {signature, sizeof(signature)});
    if (!signed_result.ok())
        return 0;
    out.vector(signature, signed_result.bytes);
    return out.used;
}
u32 batch(ConstSlice<u8> first, ConstSlice<u8> second, u8* output) noexcept {
    Out out{output, 0};
    out.integer(0, 1);
    out.varint(first.count + second.count);
    out.bytes(first.data, first.count);
    out.bytes(second.data, second.count);
    return out.used;
}
struct Member {
    DaveSession session;
    u64 user_id;
    u8 key_package[kMlsKeyPackageMaximum];
    u32 key_package_bytes;
};
struct Fixture {
    Member alice, bob, carol;
    u8 es[256], batch[8192], saved[65536], frame[2048], output[2048], scratch[2048];
    u32 es_bytes;
};
bool join(Member& member, const u8* es, u32 es_bytes, u64 group_id) noexcept {
    ConstSlice<u8> package{};
    const bool ok =
        dave_session_initialize(&member.session, group_id, member.user_id, 77, 1) == MlsStatus::ok &&
        dave_session_set_external_sender(&member.session, {es, es_bytes}) == MlsStatus::ok &&
        dave_session_key_package(&member.session, &package) == MlsStatus::ok;
    if (ok) {
        copy_bytes(member.key_package, package.data, package.count);
        member.key_package_bytes = package.count;
    }
    return ok;
}
bool rekey(Member& member) noexcept {
    ConstSlice<u8> package{};
    if (dave_session_key_package(&member.session, &package) != MlsStatus::ok)
        return false;
    copy_bytes(member.key_package, package.data, package.count);
    member.key_package_bytes = package.count;
    return true;
}
// Encrypt an Opus-sized frame as one member and decrypt it as another.
bool exchange(Fixture& f, Member& from, Member& to, u8 tag, u64 now) noexcept {
    u8 opus[160];
    for (u32 i = 0; i != sizeof(opus); ++i)
        opus[i] = static_cast<u8>(i * 31 + tag);
    const auto sealed =
        dave_session_encrypt(&from.session, {opus, sizeof(opus)}, {f.frame, sizeof(f.frame)});
    if (!sealed.ok())
        return false;
    const auto opened = dave_session_decrypt(&to.session, from.user_id, {f.frame, sealed.bytes}, now,
                                             {f.scratch, sizeof(f.scratch)}, {f.output, sizeof(f.output)});
    return opened.ok() && opened.bytes == sizeof(opus) && bytes_equal(f.output, opus, sizeof(opus));
}
bool execute_all(Member** members, u32 count, u16 transition, u64 now) noexcept {
    bool ok = true;
    for (u32 i = 0; i != count; ++i)
        ok = ok && dave_session_execute_transition(&members[i]->session, transition, now) == MlsStatus::ok;
    return ok;
}

// One gateway round: every current member caches the proposals, the chosen
// committer's Commit is announced to all, and joiners take its Welcome.
struct Round {
    Member** members;
    u32 count, committer;
    Member** joiners;
    u32 joiner_count;
    ConstSlice<u64> recognized;
};
MlsStatus play(Fixture& f, const Round& round, ConstSlice<u8> proposals, u16 transition,
               MlsStatus* statuses) noexcept {
    ConstSlice<u8> output{};
    u32 commit_bytes = 0;
    u32 saved = 0;
    for (u32 i = 0; i != round.count; ++i) {
        const auto status = dave_session_proposals(&round.members[i]->session, proposals, round.recognized,
                                                   &output, &commit_bytes);
        if (i == round.committer) {
            if (status != MlsStatus::ok)
                return status;
            saved = output.count;
            copy_bytes(f.saved, output.data, output.count);
        }
    }
    const u32 commit = round.members[round.committer]->session.outbound_commit_bytes;
    for (u32 i = 0; i != round.count; ++i)
        statuses[i] = dave_session_commit(&round.members[i]->session, transition, {f.saved, commit});
    for (u32 i = 0; i != round.joiner_count; ++i)
        if (const auto status = dave_session_welcome(&round.joiners[i]->session, transition,
                                                     {f.saved + commit, saved - commit}, round.recognized);
            status != MlsStatus::ok)
            return status;
    return MlsStatus::ok;
}
u32 batch_of(ConstSlice<ConstSlice<u8>> items, u8* output) noexcept {
    Out out{output, 0};
    u32 total = 0;
    for (u32 i = 0; i != items.count; ++i)
        total += items[i].count;
    out.integer(0, 1);
    out.varint(total);
    for (u32 i = 0; i != items.count; ++i)
        out.bytes(items[i].data, items[i].count);
    return out.used;
}
u32 unmerged_at(const DaveSession& session, u32 node) noexcept {
    auto iterator = mls_tree_iterate(session.group.active.tree);
    MlsTreeNode current{};
    bool has = false;
    while (mls_tree_next(iterator, &current, &has) == MlsStatus::ok && has)
        if (current.index == node)
            return current.present && current.type == kMlsNodeParent ? current.parent.unmerged_count : 0;
    return 0;
}

// Four members: a path Commit, a no-path Add that leaves the joiner as an
// unmerged leaf of the root, then a path Commit the former unmerged joiner
// must decrypt. Exercises unmerged-leaf validation and resolution.
void unmerged(Check check, KdfSha256Workspace& kdf) noexcept {
    auto* f = static_cast<Fixture*>(
        VirtualAlloc(nullptr, sizeof(Fixture), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* extra = static_cast<Member*>(
        VirtualAlloc(nullptr, 2 * sizeof(Member), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!f || !extra) {
        check(false, "DAVE unmerged fixture allocation");
        return;
    }
    constexpr u64 group = 5550001ull;
    P256Key gateway{};
    u8 gateway_public[65], items[3][1024];
    const u8 identity[]{'d', 'a', 'v', 'e'};
    p256_generate(&gateway, P256Kind::ecdsa);
    p256_export_public(gateway, {gateway_public, 65});
    write_external_sender(gateway_public, {identity, 4}, {f->es, sizeof(f->es)}, &f->es_bytes);
    Member& a = f->alice;
    Member& b = f->bob;
    Member& c = f->carol;
    Member& d = extra[0];
    Member& e = extra[1];
    a.user_id = 11;
    b.user_id = 12;
    c.user_id = 13;
    d.user_id = 14;
    e.user_id = 15;
    const u64 all[]{11, 12, 13, 14, 15};
    bool ready = true;
    Member* everyone[]{&a, &b, &c, &d};
    for (auto* member : everyone)
        ready = ready && join(*member, f->es, f->es_bytes, group);
    ConstSlice<u8> adds[3];
    for (u32 i = 0; i != 3; ++i)
        adds[i] = {items[i], proposal(kdf, gateway, group, 0,
                                      {everyone[i + 1]->key_package, everyone[i + 1]->key_package_bytes}, 0,
                                      items[i])};
    u32 bytes = batch_of({adds, 3}, f->batch);
    MlsStatus statuses[5]{};
    Member* founders[]{&a};
    Member* joiners[]{&b, &c, &d};
    check(ready &&
              play(*f, {founders, 1, 0, joiners, 3, {all, 4}}, {f->batch, bytes}, 1, statuses) ==
                  MlsStatus::ok &&
              statuses[0] == MlsStatus::ok && execute_all(everyone, 4, 1, 100),
          "one Commit adds three members who all join from one Welcome");

    const ConstSlice<u8> remove_d{items[0], proposal(kdf, gateway, group, 1, {}, 3, items[0])};
    bytes = batch_of({&remove_d, 1}, f->batch);
    check(play(*f, {everyone, 4, 2, nullptr, 0, {all, 4}}, {f->batch, bytes}, 2, statuses) ==
                  MlsStatus::ok &&
              statuses[0] == MlsStatus::ok && statuses[1] == MlsStatus::ok &&
              statuses[2] == MlsStatus::ok && statuses[3] == MlsStatus::member_removed,
          "a path Commit removes the fourth member");
    Member* three[]{&a, &b, &c};
    check(execute_all(three, 3, 2, 200) && exchange(*f, a, c, 1, 201),
          "the three remaining members share epoch 2");

    check(join(e, f->es, f->es_bytes, group), "a new member prepares to join");
    const ConstSlice<u8> add_e{
        items[0], proposal(kdf, gateway, group, 2, {e.key_package, e.key_package_bytes}, 0, items[0])};
    bytes = batch_of({&add_e, 1}, f->batch);
    Member* newcomer[]{&e};
    const u64 present[]{11, 12, 13, 15};
    check(play(*f, {three, 3, 1, newcomer, 1, {present, 4}}, {f->batch, bytes}, 3, statuses) ==
                  MlsStatus::ok &&
              statuses[0] == MlsStatus::ok && statuses[1] == MlsStatus::ok && statuses[2] == MlsStatus::ok,
          "a no-path Add places the joiner under the committed root");
    Member* four[]{&a, &b, &c, &e};
    check(execute_all(four, 4, 3, 300) && unmerged_at(a.session, 3) == 1 &&
              unmerged_at(e.session, 3) == 1 && exchange(*f, c, e, 2, 301) && exchange(*f, e, a, 3, 302),
          "the joiner is an unmerged leaf of the root and every member validates that tree");

    const ConstSlice<u8> remove_b{items[0], proposal(kdf, gateway, group, 3, {}, 1, items[0])};
    bytes = batch_of({&remove_b, 1}, f->batch);
    check(play(*f, {four, 4, 2, nullptr, 0, {present, 4}}, {f->batch, bytes}, 4, statuses) ==
                  MlsStatus::ok &&
              statuses[0] == MlsStatus::ok && statuses[1] == MlsStatus::member_removed &&
              statuses[2] == MlsStatus::ok && statuses[3] == MlsStatus::ok,
          "a path Commit encrypts to the former unmerged leaf");
    Member* remaining[]{&a, &c, &e};
    check(execute_all(remaining, 3, 4, 400) && !unmerged_at(e.session, 3) && exchange(*f, e, c, 4, 401) &&
              exchange(*f, a, e, 5, 402) &&
              bytes_equal(e.session.group.active.epoch.exporter_secret,
                          a.session.group.active.epoch.exporter_secret, 32),
          "the former unmerged joiner derives the new path and epoch");
    Member* members[]{&a, &b, &c, &d, &e};
    for (auto* member : members)
        dave_session_release(&member->session);
    p256_release(&gateway);
    VirtualFree(extra, 0, MEM_RELEASE);
    VirtualFree(f, 0, MEM_RELEASE);
}
void session(Check check, KdfSha256Workspace& kdf) noexcept {
    auto* f = static_cast<Fixture*>(
        VirtualAlloc(nullptr, sizeof(Fixture), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!f) {
        check(false, "DAVE session fixture allocation");
        return;
    }
    constexpr u64 group = 1234567890123456789ull;
    f->alice.user_id = 1111;
    f->bob.user_id = 2222;
    f->carol.user_id = 3333;
    P256Key gateway{};
    u8 gateway_public[65];
    const u8 identity[]{'d', 'a', 'v', 'e'};
    check(p256_generate(&gateway, P256Kind::ecdsa).ok() &&
              p256_export_public(gateway, {gateway_public, 65}).ok() &&
              write_external_sender(gateway_public, {identity, 4}, {f->es, sizeof(f->es)},
                                    &f->es_bytes) == MlsStatus::ok,
          "DAVE test gateway ExternalSender");
    auto& alice = f->alice;
    auto& bob = f->bob;
    auto& carol = f->carol;
    check(join(alice, f->es, f->es_bytes, group) && join(bob, f->es, f->es_bytes, group) &&
              alice.session.group_state == DaveGroupState::initial &&
              bob.session.group_state == DaveGroupState::initial &&
              !dave_session_can_encrypt(alice.session),
          "DAVE members form pending one-member groups from KeyPackage and ExternalSender");
    check(dave_session_set_external_sender(&alice.session, {gateway_public, 65}) ==
              MlsStatus::state_conflict,
          "a different external sender cannot replace one bound into the pending group");

    // Epoch 0 -> 1: both pending members race; the gateway picks Alice's.
    u8 proposal_a[1024], proposal_b[1024];
    const u32 add_bob =
        proposal(kdf, gateway, group, 0, {bob.key_package, bob.key_package_bytes}, 0, proposal_a);
    const u32 add_alice =
        proposal(kdf, gateway, group, 0, {alice.key_package, alice.key_package_bytes}, 0, proposal_b);
    const u64 both[]{alice.user_id, bob.user_id};
    ConstSlice<u8> output{};
    u32 commit_bytes = 0;
    u32 batch_bytes = batch({proposal_b, add_alice}, {}, f->batch);
    check(dave_session_proposals(&bob.session, {f->batch, batch_bytes}, {both, 2}, &output,
                                 &commit_bytes) == MlsStatus::ok,
          "a pending member authors a candidate Commit for gateway proposals");
    batch_bytes = batch({proposal_a, add_bob}, {}, f->batch);
    const u64 alice_only[]{alice.user_id};
    check(dave_session_proposals(&alice.session, {f->batch, batch_bytes}, {alice_only, 1}, &output,
                                 &commit_bytes) == MlsStatus::member_removed,
          "an Add for a user the voice gateway did not announce is rejected");
    check(dave_session_proposals(&alice.session, {f->batch, batch_bytes}, {both, 2}, &output,
                                 &commit_bytes) == MlsStatus::ok &&
              commit_bytes && output.count > commit_bytes,
          "the committer produces Commit plus Welcome for an Add");
    copy_bytes(f->saved, output.data, output.count);
    const ConstSlice<u8> commit1{f->saved, commit_bytes};
    const ConstSlice<u8> welcome1{f->saved + commit_bytes, output.count - commit_bytes};
    check(dave_session_commit(&alice.session, 1, commit1) == MlsStatus::ok &&
              dave_session_transition_prepared(alice.session, 1),
          "the announced echo of our own Commit binds its transition");
    check(dave_session_welcome(&bob.session, 1, welcome1, {alice_only, 1}) == MlsStatus::member_removed &&
              bob.session.group_state == DaveGroupState::initial &&
              !dave_session_transition_prepared(bob.session, 1),
          "a Welcome whose tree holds an unannounced user is refused without side effects");
    u8 tampered[8192];
    copy_bytes(tampered, welcome1.data, welcome1.count);
    tampered[welcome1.count - 1] ^= 1;
    check(dave_session_welcome(&bob.session, 1, {tampered, welcome1.count}, {both, 2}) ==
                  MlsStatus::crypto_failure &&
              bob.session.group_state == DaveGroupState::initial,
          "a Welcome with a corrupted GroupInfo tag is rejected");
    check(dave_session_welcome(&bob.session, 1, welcome1, {both, 2}) == MlsStatus::ok &&
              bob.session.group_state == DaveGroupState::established,
          "the joiner decrypts the Welcome, verifies GroupInfo, tree and confirmation, and replaces its "
          "candidate");
    Member* pair[]{&alice, &bob};
    check(execute_all(pair, 2, 1, 1000) && dave_session_can_encrypt(alice.session) &&
              dave_session_can_encrypt(bob.session) && alice.session.group.active.context.epoch == 1 &&
              bob.session.group.active.context.epoch == 1 &&
              bytes_equal(alice.session.group.active.epoch.exporter_secret,
                          bob.session.group.active.epoch.exporter_secret, 32),
          "both members execute the transition into the same epoch-1 secrets");
    check(exchange(*f, alice, bob, 1, 1001) && exchange(*f, bob, alice, 2, 1002),
          "per-sender exported keys round-trip frames in both directions");
    const u32 nonce = alice.session.sealer.truncated_nonce;
    check(dave_session_prepare_transition(&alice.session, 50, 1, false) == MlsStatus::ok &&
              dave_session_execute_transition(&alice.session, 50, 1003) == MlsStatus::ok && nonce &&
              alice.session.sealer.truncated_nonce == nonce && exchange(*f, alice, bob, 11, 1004),
          "re-executing a transition into the keyed epoch keeps the sender's nonce sequence");
    u8 old_frame[512];
    u8 opus[64]{};
    const auto old_sealed = dave_session_encrypt(&bob.session, {opus, 64}, {old_frame, sizeof(old_frame)});

    // Epoch 1 -> 2: Carol is added; Bob's Commit wins over Alice's candidate.
    check(join(carol, f->es, f->es_bytes, group), "a third member prepares to join");
    const u32 add_carol =
        proposal(kdf, gateway, group, 1, {carol.key_package, carol.key_package_bytes}, 0, proposal_a);
    batch_bytes = batch({proposal_a, add_carol}, {}, f->batch);
    const u64 three[]{alice.user_id, bob.user_id, carol.user_id};
    check(dave_session_proposals(&alice.session, {f->batch, batch_bytes}, {three, 3}, &output,
                                 &commit_bytes) == MlsStatus::ok,
          "Alice authors a candidate for Carol's Add");
    check(dave_session_proposals(&bob.session, {f->batch, batch_bytes}, {three, 3}, &output,
                                 &commit_bytes) == MlsStatus::ok,
          "Bob authors a rival candidate for the same proposal");
    copy_bytes(f->saved, output.data, output.count);
    const ConstSlice<u8> commit2{f->saved, commit_bytes};
    const ConstSlice<u8> welcome2{f->saved + commit_bytes, output.count - commit_bytes};
    check(dave_session_commit(&bob.session, 2, commit2) == MlsStatus::ok &&
              dave_session_commit(&alice.session, 2, commit2) == MlsStatus::ok &&
              alice.session.group.pending == MlsPending::remote,
          "an authenticated rival Commit replaces the losing local candidate");
    check(dave_session_welcome(&carol.session, 2, welcome2, {three, 3}) == MlsStatus::ok,
          "Carol joins from the winning Welcome");
    Member* trio[]{&alice, &bob, &carol};
    check(execute_all(trio, 3, 2, 2000) && carol.session.group.active.context.epoch == 2 &&
              carol.session.group.active.tree_private.leaf_index == 2,
          "all three members reach epoch 2");
    check(exchange(*f, alice, carol, 3, 2001) && exchange(*f, carol, bob, 4, 2002) &&
              exchange(*f, bob, alice, 5, 2003),
          "every pair exchanges frames after an Add without an UpdatePath");
    auto opened = dave_session_decrypt(&alice.session, bob.user_id, {old_frame, old_sealed.bytes}, 2004,
                                       {f->scratch, sizeof(f->scratch)}, {f->output, sizeof(f->output)});
    check(old_sealed.ok() && opened.ok() && opened.bytes == 64,
          "a frame from the previous epoch still opens during the transition grace period");

    // Epoch 2 -> 3: Carol is removed; Alice commits with an UpdatePath that
    // Bob decrypts (TreeKEM) and Carol cannot follow.
    const auto carol_old = dave_session_encrypt(&carol.session, {opus, 64}, {old_frame, sizeof(old_frame)});
    const u32 remove_carol = proposal(kdf, gateway, group, 2, {}, 2, proposal_a);
    batch_bytes = batch({proposal_a, remove_carol}, {}, f->batch);
    check(dave_session_proposals(&alice.session, {f->batch, batch_bytes}, {both, 2}, &output,
                                 &commit_bytes) == MlsStatus::ok &&
              output.count == commit_bytes && alice.session.group.local.commit.has_path,
          "a Remove forces a path Commit without a Welcome");
    copy_bytes(f->saved, output.data, output.count);
    const ConstSlice<u8> commit3{f->saved, commit_bytes};
    check(dave_session_proposals(&bob.session, {f->batch, batch_bytes}, {both, 2}, &output,
                                 &commit_bytes) == MlsStatus::ok,
          "Bob caches the Remove proposal");
    check(dave_session_proposals(&carol.session, {f->batch, batch_bytes}, {three, 3}, &output,
                                 &commit_bytes) == MlsStatus::malformed,
          "the removed member cannot commit its own removal");
    u8 corrupted[8192];
    copy_bytes(corrupted, commit3.data, commit3.count);
    corrupted[commit3.count - 40] ^= 0x10;
    check(dave_session_commit(&bob.session, 3, {corrupted, commit3.count}) != MlsStatus::ok &&
              bob.session.group.pending == MlsPending::local && exchange(*f, alice, bob, 6, 2500),
          "a tampered Commit is rejected while Bob's own candidate and keys survive");
    check(dave_session_commit(&alice.session, 3, commit3) == MlsStatus::ok &&
              dave_session_commit(&bob.session, 3, commit3) == MlsStatus::ok &&
              dave_session_commit(&carol.session, 3, commit3) == MlsStatus::member_removed,
          "the path Commit is applied by Bob and reports removal to Carol");
    check(execute_all(pair, 2, 3, 3000) && alice.session.group.active.context.epoch == 3 &&
              bytes_equal(alice.session.group.active.epoch.exporter_secret,
                          bob.session.group.active.epoch.exporter_secret, 32) &&
              exchange(*f, alice, bob, 7, 3001) && exchange(*f, bob, alice, 8, 3002),
          "after UpdatePath decryption both remaining members share epoch 3");
    opened = dave_session_decrypt(&bob.session, carol.user_id, {old_frame, carol_old.bytes}, 3003,
                                  {f->scratch, sizeof(f->scratch)}, {f->output, sizeof(f->output)});
    const bool grace = carol_old.ok() && opened.ok();
    opened = dave_session_decrypt(&bob.session, carol.user_id, {old_frame, carol_old.bytes},
                                  3000 + kDaveEpochGraceMs + 1, {f->scratch, sizeof(f->scratch)},
                                  {f->output, sizeof(f->output)});
    check(grace && !opened.ok(), "a removed member's keys expire after the grace period");

    // Invalid-commit recovery: Bob abandons the group and rejoins through a
    // Remove(old leaf) + Add(new KeyPackage) cycle authored by Alice.
    check(dave_session_reinitialize(&bob.session, 1) == MlsStatus::ok && rekey(bob) &&
              bob.session.group_state == DaveGroupState::initial,
          "recovery reinitializes a pending group with a fresh KeyPackage");
    const u32 remove_bob = proposal(kdf, gateway, group, 3, {}, 1, proposal_a);
    const u32 readd_bob =
        proposal(kdf, gateway, group, 3, {bob.key_package, bob.key_package_bytes}, 0, proposal_b);
    batch_bytes = batch({proposal_a, remove_bob}, {proposal_b, readd_bob}, f->batch);
    check(dave_session_proposals(&alice.session, {f->batch, batch_bytes}, {both, 2}, &output,
                                 &commit_bytes) == MlsStatus::ok &&
              output.count > commit_bytes,
          "Alice re-adds the recovering member with a path Commit and Welcome");
    copy_bytes(f->saved, output.data, output.count);
    check(dave_session_commit(&alice.session, 4, {f->saved, commit_bytes}) == MlsStatus::ok &&
              dave_session_welcome(&bob.session, 4, {f->saved + commit_bytes, output.count - commit_bytes},
                                   {both, 2}) == MlsStatus::ok &&
              execute_all(pair, 2, 4, 20000) && exchange(*f, alice, bob, 9, 20001) &&
              exchange(*f, bob, alice, 10, 20002),
          "the recovered member joins the next epoch and media resumes");

    // Downgrade to transport-only media.
    check(dave_session_prepare_transition(&alice.session, 9, 0, false) == MlsStatus::ok &&
              dave_session_execute_transition(&alice.session, 9, 21000) == MlsStatus::ok &&
              dave_session_can_encrypt(alice.session) && !alice.session.group.initialized,
          "a version-zero transition drops DAVE keys and permits passthrough");
    const auto plain = dave_session_encrypt(&alice.session, {opus, 64}, {f->frame, sizeof(f->frame)});
    opened = dave_session_decrypt(&alice.session, bob.user_id, {f->frame, 64}, 21001,
                                  {f->scratch, sizeof(f->scratch)}, {f->output, sizeof(f->output)});
    check(plain.ok() && plain.bytes == 64 && bytes_equal(f->frame, opus, 64) && opened.ok() &&
              opened.passthrough,
          "passthrough media is copied unchanged");
    check(dave_session_execute_transition(&bob.session, 77, 21002) == MlsStatus::state_conflict,
          "an unprepared transition cannot execute");

    dave_session_release(&alice.session);
    dave_session_release(&bob.session);
    dave_session_release(&carol.session);
    p256_release(&gateway);
    VirtualFree(f, 0, MEM_RELEASE);
}

const SecureBuffer& outbox(const VoiceConnection& connection, u32 index) noexcept {
    return connection.dave_outbox[(connection.dave_outbox_head + index) % kVoiceDaveOutbox];
}
bool queued_json(const VoiceConnection& connection, u32 index, const char* expected) noexcept {
    u32 length = 0;
    while (expected[length])
        ++length;
    const auto& buffer = outbox(connection, index);
    return index < connection.dave_outbox_count &&
           !connection.dave_outbox_binary[(connection.dave_outbox_head + index) % kVoiceDaveOutbox] &&
           buffer.bytes == length && bytes_equal(buffer.data, expected, length);
}
bool queued_binary(const VoiceConnection& connection, u32 index, u8 opcode) noexcept {
    return index < connection.dave_outbox_count &&
           connection.dave_outbox_binary[(connection.dave_outbox_head + index) % kVoiceDaveOutbox] &&
           outbox(connection, index).bytes > 1 && outbox(connection, index).data[0] == opcode;
}
// Drops every queued reply, as a flush to the socket would.
void drain(VoiceConnection& connection) noexcept {
    for (auto& buffer : connection.dave_outbox)
        secure_buffer_release(&buffer);
    connection.dave_outbox_head = connection.dave_outbox_count = 0;
}
struct LogCapture {
    u32 records, errors, invalid_reports, reconnects;
    bool clean; // No record carried a quote, colon or address-like text.
};
LogCapture g_log_capture;
bool record_has(const char* record, const char* text) noexcept {
    for (const char* at = record; *at; ++at) {
        u32 i = 0;
        while (text[i] && at[i] == text[i])
            ++i;
        if (!text[i])
            return true;
    }
    return false;
}
void capture_record(const char* record) noexcept {
    auto& capture = g_log_capture;
    ++capture.records;
    if (record[6] == 'E')
        ++capture.errors;
    if (record_has(record, "dave_invalid_commit_welcome_sent"))
        ++capture.invalid_reports;
    if (record_has(record, "dave_reconnect_requested"))
        ++capture.reconnects;
    for (const char* at = record + 8; *at; ++at)
        if (*at == '"' || *at == ':' || *at == '.')
            capture.clean = false;
}
// A scripted voice-gateway conversation through the socket event handler:
// synthetic JSON and binary (sequence + opcode) frames, from being the sole
// member through a DAVE member joining, a bot without DAVE forcing a
// downgrade and leaving again, and invalid-commit recovery. Membership and
// DAVE events never end the call.
void voice_signaling(Check check, KdfSha256Workspace& kdf) noexcept {
    auto* c = static_cast<VoiceConnection*>(
        VirtualAlloc(nullptr, sizeof(VoiceConnection), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* bob = static_cast<Member*>(
        VirtualAlloc(nullptr, sizeof(Member), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* wire = static_cast<u8*>(VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!c || !bob || !wire) {
        check(false, "DAVE voice signaling fixture allocation");
        return;
    }
    g_log_capture = {0, 0, 0, 0, true};
    voice_log_set_sink(capture_record);
    constexpr u64 group = 987654321ull;
    P256Key gateway{};
    u8 gateway_public[65], es[128], message[1024], batched[1024], payload[8192];
    u32 es_bytes = 0, scratch[256];
    const u8 identity[]{'d', 'a', 'v', 'e'};
    p256_generate(&gateway, P256Kind::ecdsa);
    p256_export_public(gateway, {gateway_public, 65});
    write_external_sender(gateway_public, {identity, 4}, {es, sizeof(es)}, &es_bytes);
    c->dave_group_id = group;
    c->dave_user_id = 1111;
    c->socket.setup_generation = 9;
    c->socket.phase = VoiceSocketPhase::open;
    c->socket.session.scratch = {scratch, 256};
    c->socket.now_ms = 100;
    c->identify_submitted = true;
    c->media.dave_protocol_version = 1;
    u16 sequence = 0;
    const auto json = [&](const char* text) noexcept {
        u32 length = 0;
        while (text[length])
            ++length;
        VoiceSessionEvent event{};
        event.kind = VoiceSessionEventKind::json;
        if (!voice_gateway_decode_json({text, length}, {scratch, 256}, 64, &event.envelope).ok())
            return false;
        return voice_connection_event(c, event);
    };
    const auto binary = [&](VoiceDaveOpcode opcode, ConstSlice<u8> data) noexcept {
        ++sequence;
        wire[0] = static_cast<u8>(sequence >> 8);
        wire[1] = static_cast<u8>(sequence);
        wire[2] = static_cast<u8>(opcode);
        copy_bytes(wire + 3, data.data, data.count);
        VoiceSessionEvent event{};
        event.kind = VoiceSessionEventKind::binary;
        return voice_gateway_decode_binary({wire, data.count + 3}, &event.binary) &&
               event.binary.sequence == sequence && voice_connection_event(c, event);
    };
    // The announced commit (opcode 29) is our own queued opcode 28 Commit.
    const auto announce = [&](u16 transition, u32 index, u32 commit_bytes) noexcept {
        payload[0] = static_cast<u8>(transition >> 8);
        payload[1] = static_cast<u8>(transition);
        copy_bytes(payload + 2, outbox(*c, index).data + 1, commit_bytes);
        return binary(VoiceDaveOpcode::mls_announce_commit_transition, {payload, commit_bytes + 2});
    };
    const auto healthy = [&]() noexcept { return c->error == VoiceConnectionError::none; };

    check(binary(VoiceDaveOpcode::mls_external_sender, {es, es_bytes}) && !c->dave_session.initialized &&
              c->dave_external_sender_bytes == es_bytes,
          "an ExternalSender before Session Description is retained");
    check(voice_connection_dave_description(c, 100) && c->dave_session.initialized &&
              c->dave_session.group_state == DaveGroupState::initial && !c->dave_external_sender_bytes &&
              c->dave_outbox_count == 1 && queued_binary(*c, 0, 26),
          "sole member: Session Description creates our one-member group and queues opcode 26");
    drain(*c);

    // A DAVE member (the bot) connects: its announcements, whatever optional
    // fields it leaves null or malformed, never end the call.
    check(json(R"({"op":11,"seq":1,"d":{"user_ids":["2222"]}})") &&
              json(R"({"op":18,"seq":2,"d":{"user_id":"2222","flags":null}})") &&
              json(R"({"op":20,"seq":3,"d":{"user_id":"2222","platform":null}})") &&
              json(R"({"op":12,"seq":4,"d":{"user_id":"2222","audio_ssrc":5,"video_ssrc":0,"streams":null}})") &&
              json(R"({"op":12,"seq":5,"d":{"user_id":"2222","audio_ssrc":5,"streams":[null]}})") &&
              json(R"({"op":5,"seq":6,"d":{"user_id":"2222","ssrc":5,"speaking":1}})") && healthy() &&
              voice_participants_user(c->participants, 2222).connected &&
              voice_participants_source(c->participants, 5).user_id == 2222,
          "client connect, flags, platform and SSRC announcements from a bot keep the call");
    bob->user_id = 2222;
    check(join(*bob, es, es_bytes, group), "the joining member prepares its KeyPackage");
    u32 add = proposal(kdf, gateway, group, 0, {bob->key_package, bob->key_package_bytes}, 0, message);
    u32 batch_bytes = batch({message, add}, {}, batched);
    check(binary(VoiceDaveOpcode::mls_proposals, {batched, batch_bytes}) && c->dave_outbox_count == 1 &&
              queued_binary(*c, 0, 28),
          "sole member commits the joiner's Add: opcode 28 carries Commit and Welcome");
    u32 commit_bytes = c->dave_session.outbound_commit_bytes;
    ConstSlice<u8> welcome{outbox(*c, 0).data + 1 + commit_bytes, outbox(*c, 0).bytes - 1 - commit_bytes};
    check(announce(5, 0, commit_bytes) && queued_json(*c, 1, R"({"op":23,"d":{"transition_id":5}})"),
          "our announced Commit is acknowledged with ready_for_transition");
    const u64 both[]{1111, 2222};
    check(dave_session_welcome(&bob->session, 5, welcome, {both, 2}) == MlsStatus::ok &&
              dave_session_execute_transition(&bob->session, 5, 100) == MlsStatus::ok,
          "the joining member processes our Welcome");
    drain(*c);
    check(json(R"({"op":22,"seq":7,"d":{"transition_id":5}})") && voice_connection_dave_ready(*c) &&
              !c->dave_recovering && c->dave_session.group.active.context.epoch == 1 &&
              bytes_equal(c->dave_session.group.active.epoch.exporter_secret,
                          bob->session.group.active.epoch.exporter_secret, 32),
          "execute_transition moves both members to epoch 1 and enables media");
    u8 opus[40]{}, sealed[128], opened[128], work[128];
    for (u32 i = 0; i != sizeof(opus); ++i)
        opus[i] = static_cast<u8>(i + 1);
    auto frame = voice_connection_dave_encrypt(c, {opus, 40}, {sealed, sizeof(sealed)});
    auto result = dave_session_decrypt(&bob->session, 1111, {sealed, frame.bytes}, 101, {work, sizeof(work)},
                                       {opened, sizeof(opened)});
    check(frame.ok() && frame.bytes > 40 && result.ok() && result.bytes == 40 && bytes_equal(opened, opus, 40),
          "our sealed frames open for the joined member");
    frame = dave_session_encrypt(&bob->session, {opus, 40}, {sealed, sizeof(sealed)});
    result = dave_session_decrypt(&c->dave_session, 2222, {sealed, frame.bytes}, 102, {work, sizeof(work)},
                                  {opened, sizeof(opened)});
    const u8 silence[]{0xf8, 0xff, 0xfe};
    const auto quiet = dave_session_decrypt(&c->dave_session, 2222, {silence, 3}, 103, {work, sizeof(work)},
                                            {opened, sizeof(opened)});
    const auto plain = dave_session_decrypt(&c->dave_session, 2222, {opus, 40}, 104, {work, sizeof(work)},
                                            {opened, sizeof(opened)});
    check(frame.ok() && result.ok() && result.bytes == 40 && quiet.ok() && quiet.passthrough &&
              !plain.ok() && plain.error == DaveMediaError::rejected,
          "the member's frames open; silence passes through; other unencrypted frames are refused");
    check(json(R"({"op":31,"seq":8,"d":{"transition_id":5}})") &&
              json(R"({"op":23,"seq":9,"d":{"transition_id":5}})") &&
              binary(VoiceDaveOpcode::mls_invalid_commit_welcome, {payload, 2}) &&
              json(R"({"op":22,"seq":10,"d":{"transition_id":"x"}})") &&
              binary(VoiceDaveOpcode::mls_welcome, {payload, 2}) &&
              json(R"({"op":22,"seq":11,"d":{"transition_id":77}})") && healthy() && !c->dave_outbox_count &&
              voice_connection_dave_ready(*c),
          "client-direction, malformed, truncated and unprepared DAVE messages are ignored");

    // A bot without DAVE joins: the gateway asks for a downgrade, which is
    // refused as the official client does (close 4804, fresh session).
    check(json(R"({"op":11,"seq":12,"d":{"user_ids":["3333"]}})") &&
              json(R"({"op":21,"seq":13,"d":{"transition_id":6,"protocol_version":0}})") && healthy() &&
              c->dave_close_code == kVoiceCloseDaveDowngradeRefused && c->dave_reconnect_requested &&
              !c->dave_outbox_count && c->dave_session.protocol_version == 1,
          "a downgrade to version 0 is refused with close 4804 and a reconnect, not acknowledged");
    check(json(R"({"op":22,"seq":14,"d":{"transition_id":6}})") && c->dave_session.protocol_version == 1 &&
              !c->dave_outbox_count,
          "events after the refusal are not applied");
    c->dave_close_code = 0;
    c->dave_reconnect_requested = false;
    c->dave_reconnects = 0;
    check(json(R"({"op":24,"seq":15,"d":{"protocol_version":0,"epoch":2}})") &&
              c->dave_close_code == kVoiceCloseDaveDowngradeRefused,
          "a version-0 epoch is refused too");
    c->dave_close_code = 0;
    c->dave_reconnect_requested = false;
    c->dave_reconnects = 0;
    // The member set changes again: the gateway announces a new group (epoch 1).
    check(json(R"({"op":13,"seq":16,"d":{"user_id":"3333"}})") &&
              !voice_participants_user(c->participants, 3333).user_id &&
              json(R"({"op":24,"seq":17,"d":{"protocol_version":1,"epoch":1}})") && c->dave_outbox_count == 1 &&
              queued_binary(*c, 0, 26) && c->dave_session.protocol_version == 1,
          "epoch 1 rebuilds the group with a fresh opcode 26");
    drain(*c);
    check(dave_session_reinitialize(&bob->session, 1) == MlsStatus::ok && rekey(*bob),
          "the DAVE member rejoins the new group");
    add = proposal(kdf, gateway, group, 0, {bob->key_package, bob->key_package_bytes}, 0, message);
    batch_bytes = batch({message, add}, {}, batched);
    check(binary(VoiceDaveOpcode::mls_proposals, {batched, batch_bytes}) && queued_binary(*c, 0, 28),
          "we commit the member into the new group");
    commit_bytes = c->dave_session.outbound_commit_bytes;
    welcome = {outbox(*c, 0).data + 1 + commit_bytes, outbox(*c, 0).bytes - 1 - commit_bytes};
    check(announce(7, 0, commit_bytes) && queued_json(*c, 1, R"({"op":23,"d":{"transition_id":7}})") &&
              dave_session_welcome(&bob->session, 7, welcome, {both, 2}) == MlsStatus::ok &&
              dave_session_execute_transition(&bob->session, 7, 1000) == MlsStatus::ok,
          "the new group's Commit is acknowledged and its Welcome processed");
    drain(*c);
    c->socket.now_ms = 1000;
    check(json(R"({"op":22,"seq":18,"d":{"transition_id":7}})") && voice_connection_dave_ready(*c) &&
              c->dave_session.active_protocol_version == 1,
          "the new group executes with the member");
    frame = voice_connection_dave_encrypt(c, {opus, 40}, {sealed, sizeof(sealed)});
    result = dave_session_decrypt(&bob->session, 1111, {sealed, frame.bytes}, 1001, {work, sizeof(work)},
                                  {opened, sizeof(opened)});
    const auto plain_again = dave_session_decrypt(&c->dave_session, 2222, {opus, 40}, 1002,
                                                  {work, sizeof(work)}, {opened, sizeof(opened)});
    check(frame.ok() && frame.bytes > 40 && result.ok() && result.bytes == 40 && !plain_again.ok(),
          "the new epoch encrypts; no plaintext window exists without a version-0 phase");

    // Invalid commit: opcode 31 and a fresh key package, then a failing
    // Welcome while rejoining is reported again rather than ending the call.
    payload[0] = 0;
    payload[1] = 8;
    for (u32 i = 2; i != 66; ++i)
        payload[i] = static_cast<u8>(i * 7);
    check(binary(VoiceDaveOpcode::mls_announce_commit_transition, {payload, 66}) && c->dave_recovering &&
              queued_json(*c, 0, R"({"op":31,"d":{"transition_id":8}})") && queued_binary(*c, 1, 26) &&
              c->dave_session.group_state == DaveGroupState::initial && healthy(),
          "an invalid Commit reports opcode 31 and rejoins with a fresh KeyPackage");
    drain(*c);
    check(binary(VoiceDaveOpcode::mls_announce_commit_transition, {payload, 66}) && !c->dave_outbox_count,
          "while rejoining, commits of the abandoned group are ignored");
    payload[1] = 9;
    check(binary(VoiceDaveOpcode::mls_welcome, {payload, 66}) &&
              queued_json(*c, 0, R"({"op":31,"d":{"transition_id":9}})") && queued_binary(*c, 1, 26) && healthy(),
          "a failing Welcome while rejoining is reported again instead of disconnecting");
    drain(*c);
    bool reported = true;
    for (u32 i = 0; i != 2; ++i) {
        reported = reported && binary(VoiceDaveOpcode::mls_welcome, {payload, 66});
        drain(*c);
    }
    check(reported && c->dave_invalid_streak == kVoiceDaveInvalidLimit - 1 && !c->dave_reconnect_requested,
          "consecutive invalid reports are counted");
    check(binary(VoiceDaveOpcode::mls_welcome, {payload, 66}) && c->dave_reconnect_requested && healthy(),
          "the fifth consecutive invalid message requests a fresh session instead of failing the call");
    check(c->dave_close_code == kVoiceCloseRepeatedMlsInvalid, "the repeated-invalid reconnect closes with 4803");
    check(g_log_capture.records > 20 && g_log_capture.invalid_reports == kVoiceDaveInvalidLimit - 1 &&
              g_log_capture.reconnects == 3 && g_log_capture.clean,
          "voice log records every DAVE step with numeric fields only");
    voice_log_set_sink(nullptr);
    voice_participants_release(&c->participants);
    c->socket.phase = VoiceSocketPhase::empty;
    c->socket.session.scratch = {};
    voice_connection_dave_reset(c);
    check(!c->dave_session.initialized && !c->dave_outbox_count && !c->dave_outbox[0].data,
          "reset drops the session and queued replies");
    dave_session_release(&bob->session);
    p256_release(&gateway);
    VirtualFree(wire, 0, MEM_RELEASE);
    VirtualFree(bob, 0, MEM_RELEASE);
    VirtualFree(c, 0, MEM_RELEASE);
}

// Independent reference media side, re-derived from a separate DAVE
// implementation's MLS crypto, ratchet and frame code on raw CNG primitives
// rather than our KDF/frame code: MLS-Exporter("Discord Secure Frames v0", LE
// u64 user id, 16), hash ratchet ExpandWithLabel("key"/"secret", BE u32
// generation), AES-128-GCM with the truncated nonce little-endian in bytes
// 8..11, an 8-byte tag, ULEB128 nonce, supplemental size and 0xFAFA.
struct ReferenceMedia {
    bool hmac(ConstSlice<u8> key, const ConstSlice<u8>* parts, u32 count, u8* output) noexcept {
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = BCryptCreateHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, &hash, nullptr, 0, const_cast<u8*>(key.data),
                                   key.count, 0) >= 0;
        for (u32 i = 0; ok && i != count; ++i)
            ok = !parts[i].count || BCryptHashData(hash, const_cast<u8*>(parts[i].data), parts[i].count, 0) >= 0;
        ok = ok && BCryptFinishHash(hash, output, 32, 0) >= 0;
        if (hash)
            BCryptDestroyHash(hash);
        return ok;
    }
    // RFC 9420 ExpandWithLabel with a single HKDF-Expand block (length <= 32).
    bool expand(ConstSlice<u8> secret, const char* label, ConstSlice<u8> context, u32 length, u8* output) noexcept {
        u8 info[128];
        u32 n = 0, label_bytes = 0;
        while (label[label_bytes])
            ++label_bytes;
        info[n++] = static_cast<u8>(length >> 8);
        info[n++] = static_cast<u8>(length);
        info[n++] = static_cast<u8>(8 + label_bytes);
        copy_bytes(info + n, "MLS 1.0 ", 8);
        n += 8;
        copy_bytes(info + n, label, label_bytes);
        n += label_bytes;
        info[n++] = static_cast<u8>(context.count);
        const u8 one = 1;
        const ConstSlice<u8> parts[]{{info, n}, context, {&one, 1}};
        u8 block[32];
        if (!hmac(secret, parts, 3, block))
            return false;
        copy_bytes(output, block, length);
        return true;
    }
    bool base_secret(const u8* exporter_secret, u64 user_id, u8* output) noexcept {
        u8 derived[32], context[8], context_hash[32];
        for (u32 i = 0; i != 8; ++i)
            context[i] = static_cast<u8>(user_id >> (8 * i));
        if (!expand({exporter_secret, 32}, "Discord Secure Frames v0", {}, 32, derived) ||
            BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, context, 8, context_hash, 32) < 0)
            return false;
        return expand({derived, 32}, "exported", {context_hash, 32}, 16, output);
    }
    bool ratchet_key(const u8* base, u32 generation, u8* key) noexcept {
        u8 secret[32];
        u32 secret_bytes = 16;
        copy_bytes(secret, base, 16);
        for (u32 current = 0;; ++current) {
            const u8 context[]{static_cast<u8>(current >> 24), static_cast<u8>(current >> 16),
                               static_cast<u8>(current >> 8), static_cast<u8>(current)};
            if (current == generation)
                return expand({secret, secret_bytes}, "key", {context, 4}, 16, key);
            u8 next[32];
            if (!expand({secret, secret_bytes}, "secret", {context, 4}, 32, next))
                return false;
            copy_bytes(secret, next, 32);
            secret_bytes = 32;
        }
    }
    // Full 16-byte GCM tag (the frame keeps its first 8 bytes).
    bool gcm(const u8* key, const u8* nonce, const u8* input, u32 bytes, u8* output, u8* tag) noexcept {
        BCRYPT_KEY_HANDLE handle = nullptr;
        if (BCryptGenerateSymmetricKey(BCRYPT_AES_GCM_ALG_HANDLE, &handle, nullptr, 0, const_cast<u8*>(key), 16,
                                       0) < 0)
            return false;
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<u8*>(nonce);
        info.cbNonce = 12;
        info.pbTag = tag;
        info.cbTag = 16;
        ULONG written = 0;
        const bool ok =
            BCryptEncrypt(handle, const_cast<u8*>(input), bytes, &info, nullptr, 0, output, bytes, &written, 0) >= 0;
        BCryptDestroyKey(handle);
        return ok && written == bytes;
    }
    // GCM keystream (counter blocks from 2) for opening without our AEAD.
    bool ctr(const u8* key, const u8* nonce, const u8* input, u32 bytes, u8* output) noexcept {
        BCRYPT_KEY_HANDLE handle = nullptr;
        if (BCryptGenerateSymmetricKey(BCRYPT_AES_ECB_ALG_HANDLE, &handle, nullptr, 0, const_cast<u8*>(key), 16,
                                       0) < 0)
            return false;
        bool ok = true;
        for (u32 offset = 0, counter = 2; ok && offset < bytes; offset += 16, ++counter) {
            u8 block[16], stream[16];
            copy_bytes(block, nonce, 12);
            block[12] = static_cast<u8>(counter >> 24);
            block[13] = static_cast<u8>(counter >> 16);
            block[14] = static_cast<u8>(counter >> 8);
            block[15] = static_cast<u8>(counter);
            ULONG written = 0;
            ok = BCryptEncrypt(handle, block, 16, nullptr, nullptr, 0, stream, 16, &written, 0) >= 0;
            for (u32 i = 0; ok && i != 16 && offset + i < bytes; ++i)
                output[offset + i] = static_cast<u8>(input[offset + i] ^ stream[i]);
        }
        BCryptDestroyKey(handle);
        return ok;
    }
    static void frame_nonce(u32 truncated, u8* nonce) noexcept {
        clear_bytes(nonce, 12);
        for (u32 i = 0; i != 4; ++i)
            nonce[8 + i] = static_cast<u8>(truncated >> (8 * i));
    }
    // The send side of the reference frame format for an Opus frame.
    u32 seal(const u8* base, u32 truncated, ConstSlice<u8> opus, u8* output) noexcept {
        u8 key[16], nonce[12], tag[16];
        frame_nonce(truncated, nonce);
        if (!ratchet_key(base, truncated >> 24, key) || !gcm(key, nonce, opus.data, opus.count, output, tag))
            return 0;
        u32 n = opus.count;
        copy_bytes(output + n, tag, 8);
        n += 8;
        u32 nonce_bytes = 0;
        for (u32 value = truncated;; value >>= 7) {
            output[n + nonce_bytes++] = static_cast<u8>((value & 0x7f) | (value > 0x7f ? 0x80 : 0));
            if (value <= 0x7f)
                break;
        }
        n += nonce_bytes;
        output[n++] = static_cast<u8>(8 + nonce_bytes + 3);
        output[n++] = 0xfa;
        output[n++] = 0xfa;
        return n;
    }
    // The receive side of the same format for an Opus frame (no clear ranges).
    u32 open(const u8* base, ConstSlice<u8> frame, u8* output) noexcept {
        if (frame.count < 12 || frame[frame.count - 1] != 0xfa || frame[frame.count - 2] != 0xfa)
            return 0;
        const u32 supplemental = frame[frame.count - 3];
        if (supplemental < 12 || supplemental > frame.count)
            return 0;
        const u32 body = frame.count - supplemental;
        u32 truncated = 0, shift = 0;
        for (u32 at = body + 8; at < frame.count - 3; ++at, shift += 7)
            truncated |= static_cast<u32>(frame[at] & 0x7f) << shift;
        u8 key[16], nonce[12], tag[16], check[2048];
        frame_nonce(truncated, nonce);
        if (!ratchet_key(base, truncated >> 24, key) || !ctr(key, nonce, frame.data, body, output) ||
            !gcm(key, nonce, output, body, check, tag) || !bytes_equal(tag, frame.data + body, 8))
            return 0;
        return body;
    }
};
// We are alone in the call; the bot's KeyPackage arrives in proposals, we
// commit + Welcome, the bot joins from the Welcome, both derive the same
// per-sender keys, and frames cross in both directions with the bot's media
// side computed the reference way.
void reference_interop(Check check, KdfSha256Workspace& kdf) noexcept {
    auto* c = static_cast<VoiceConnection*>(
        VirtualAlloc(nullptr, sizeof(VoiceConnection), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* bot = static_cast<Member*>(VirtualAlloc(nullptr, sizeof(Member), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!c || !bot) {
        check(false, "Reference interop fixture allocation");
        return;
    }
    constexpr u64 group = 1234500000ull, self = 4444, bot_id = 5555;
    P256Key gateway{};
    u8 gateway_public[65], es[128], message[1024], batched[1024], payload[8192];
    u32 es_bytes = 0;
    const u8 identity[]{'d', 'a', 'v', 'e'};
    p256_generate(&gateway, P256Kind::ecdsa);
    p256_export_public(gateway, {gateway_public, 65});
    write_external_sender(gateway_public, {identity, 4}, {es, sizeof(es)}, &es_bytes);
    c->dave_group_id = group;
    c->dave_user_id = self;
    c->socket.setup_generation = 3;
    c->media.dave_protocol_version = 1;
    VoiceParticipant users[1]{};
    users[0].user_id = bot_id;
    users[0].connected = true;
    c->participants.users = users;
    c->participants.user_count = 1;
    copy_bytes(c->dave_external_sender, es, es_bytes);
    c->dave_external_sender_bytes = es_bytes;
    // Our opcode 26 is the KeyPackage the reference implementation writes: basic
    // credential with the big-endian user id, capabilities {1}/{2}/{}/{}/{1},
    // lifetime 0..max.
    check(voice_connection_dave_description(c, 10) && queued_binary(*c, 0, 26),
          "Reference interop: we start alone and send our KeyPackage");
    const auto& kp = outbox(*c, 0);
    const u8 expected_tail[]{0x02, 0x00, 0x01, 0x02, 0x00, 0x02, 0x00, 0x00, 0x02, 0x00, 0x01, 0x01,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
                             0xff, 0xff, 0xff, 0xff, 0x00};
    // version, suite, init key, encryption key, signature key (65-byte keys
    // carry a 2-byte MLS varint length), credential type 1, 8-byte identity.
    bool layout = kp.bytes > 1 + 4 + 67 * 3 + 2 + 9 + sizeof(expected_tail) && kp.data[1] == 0 &&
                  kp.data[2] == 1 && kp.data[3] == 0 && kp.data[4] == 2 && kp.data[5] == 0x40 && kp.data[6] == 65;
    const u8* leaf = kp.data + 1 + 4 + 67;
    u8 identity_be[8];
    for (u32 i = 0; i != 8; ++i)
        identity_be[i] = static_cast<u8>(self >> (56 - 8 * i));
    layout = layout && leaf[134] == 0 && leaf[135] == 1 && leaf[136] == 8 && bytes_equal(leaf + 137, identity_be, 8) &&
             bytes_equal(leaf + 145, expected_tail, sizeof(expected_tail));
    check(layout, "Reference interop: KeyPackage leaf fields match the reference leaf writer byte for byte");
    drain(*c);
    bot->user_id = bot_id;
    check(join(*bot, es, es_bytes, group), "Reference interop: the bot prepares its KeyPackage");
    const u32 add = proposal(kdf, gateway, group, 0, {bot->key_package, bot->key_package_bytes}, 0, message);
    const u32 batch_bytes = batch({message, add}, {}, batched);
    check(voice_connection_dave_binary(c, {{batched, batch_bytes}, 1, 27}, 10) && queued_binary(*c, 0, 28),
          "Reference interop: we commit the bot's Add (opcode 28 = 1-byte opcode prefix, no sequence)");
    const u32 commit_bytes = c->dave_session.outbound_commit_bytes;
    const ConstSlice<u8> welcome{outbox(*c, 0).data + 1 + commit_bytes, outbox(*c, 0).bytes - 1 - commit_bytes};
    payload[0] = 0x01; // Transition 0x0102, big-endian ahead of the commit.
    payload[1] = 0x02;
    copy_bytes(payload + 2, outbox(*c, 0).data + 1, commit_bytes);
    const u64 both[]{self, bot_id};
    check(voice_connection_dave_binary(c, {{payload, commit_bytes + 2}, 2, 29}, 10) &&
              queued_json(*c, 1, R"({"op":23,"d":{"transition_id":258}})") &&
              dave_session_welcome(&bot->session, 0x0102, welcome, {both, 2}) == MlsStatus::ok &&
              dave_session_execute_transition(&bot->session, 0x0102, 10) == MlsStatus::ok &&
              dave_session_execute_transition(&c->dave_session, 0x0102, 10) == MlsStatus::ok,
          "Reference interop: the bot joins from our Welcome and both execute transition 258");
    const u8* ours = c->dave_session.group.active.epoch.exporter_secret;
    const u8* theirs = bot->session.group.active.epoch.exporter_secret;
    ReferenceMedia reference{};
    u8 our_base[16], bot_view_of_us[16], bot_base[16], our_seed[16];
    check(bytes_equal(ours, theirs, 32) && reference.base_secret(theirs, self, bot_view_of_us) &&
              reference.base_secret(theirs, bot_id, bot_base) && reference.base_secret(ours, self, our_base) &&
              dave_export_media_seed(kdf, {ours, 32}, self, {our_seed, 16}).ok() &&
              bytes_equal(our_seed, bot_view_of_us, 16) && bytes_equal(our_base, our_seed, 16),
          "Reference interop: identical epoch exporter and per-sender base secrets on both sides");
    u8 opus[120], sealed[256], opened[256], work[256];
    for (u32 i = 0; i != sizeof(opus); ++i)
        opus[i] = static_cast<u8>(i * 13 + 5);
    bool forward = true;
    for (u32 i = 0; forward && i != 3; ++i) {
        const auto frame = voice_connection_dave_encrypt(c, {opus, sizeof(opus)}, {sealed, sizeof(sealed)});
        forward = frame.ok() && reference.open(bot_view_of_us, {sealed, frame.bytes}, opened) == sizeof(opus) &&
                  bytes_equal(opened, opus, sizeof(opus));
    }
    check(forward, "Reference interop: frames we seal open with the reference derivation and format");
    bool backward = true;
    // Multi-byte ULEB128 nonces; generation 1 would need a day of frames
    // (the libdave-equivalent generation lifetime check refuses it earlier).
    const u32 nonces[]{1, 2, 300, 70000};
    for (u32 nonce : nonces) {
        const u32 bytes = reference.seal(bot_base, nonce, {opus, sizeof(opus)}, sealed);
        const auto result = dave_session_decrypt(&c->dave_session, bot_id, {sealed, bytes}, 11, {work, sizeof(work)},
                                                 {opened, sizeof(opened)});
        backward = backward && bytes && result.ok() && !result.passthrough && result.bytes == sizeof(opus) &&
                   bytes_equal(opened, opus, sizeof(opus));
    }
    check(backward, "Reference interop: reference-format frames from the bot open on our side across generations");
    c->participants.users = nullptr;
    c->participants.user_count = 0;
    voice_connection_dave_reset(c);
    dave_session_release(&bot->session);
    p256_release(&gateway);
    VirtualFree(bot, 0, MEM_RELEASE);
    VirtualFree(c, 0, MEM_RELEASE);
}

void signaling(Check check) noexcept {
    u8 text[64];
    auto written =
        voice_dave_write_transition(VoiceDaveOpcode::transition_ready, 65535, {text, sizeof(text)});
    constexpr char ready[] = R"({"op":23,"d":{"transition_id":65535}})";
    check(written.ok() && written.bytes == sizeof(ready) - 1 && bytes_equal(text, ready, written.bytes),
          "ready_for_transition (23) is encoded exactly");
    written =
        voice_dave_write_transition(VoiceDaveOpcode::mls_invalid_commit_welcome, 0, {text, sizeof(text)});
    constexpr char invalid[] = R"({"op":31,"d":{"transition_id":0}})";
    check(written.ok() && written.bytes == sizeof(invalid) - 1 &&
              bytes_equal(text, invalid, written.bytes) &&
              voice_dave_write_transition(VoiceDaveOpcode::transition_ready, 1, {text, 10}).error ==
                  GatewayCommandError::output_too_small,
          "invalid_commit_welcome (31) is encoded exactly and short output is refused");
    u32 scratch[64];
    const auto decode = [&](u8 opcode, const char* json, u32 length, VoiceDaveCommand* command) noexcept {
        const JsonValue value{{json, length}, 0, JsonValueKind::object};
        return voice_dave_decode_command(opcode, value, {scratch, 64}, 8, command).ok();
    };
    VoiceDaveCommand command{};
    constexpr char prepare[] = R"({"transition_id":12,"protocol_version":1})";
    constexpr char epoch[] = R"({"protocol_version":1,"epoch":1})";
    constexpr char missing[] = R"({"protocol_version":1})";
    constexpr char duplicate[] = R"({"transition_id":1,"transition_id":2})";
    constexpr char large[] = R"({"transition_id":65536})";
    check(decode(21, prepare, sizeof(prepare) - 1, &command) && command.transition_id == 12 &&
              command.protocol_version == 1 && decode(24, epoch, sizeof(epoch) - 1, &command) &&
              command.epoch == 1 && !command.has_transition &&
              !decode(21, missing, sizeof(missing) - 1, &command) &&
              !decode(22, duplicate, sizeof(duplicate) - 1, &command) &&
              !decode(22, large, sizeof(large) - 1, &command),
          "DAVE JSON commands decode required fields and reject missing, duplicate or oversized values");
}
} // namespace

void test_dave_session(Check check) noexcept {
    tree_math(check);
    signaling(check);
    KdfSha256Workspace kdf{};
    if (!kdf_sha256_initialize(&kdf).ok()) {
        check(false, "DAVE session test KDF workspace");
        return;
    }
    sealer(check, kdf);
    session(check, kdf);
    unmerged(check, kdf);
    voice_signaling(check, kdf);
    reference_interop(check, kdf);
    kdf_sha256_release(&kdf);
}
} // namespace maicord
