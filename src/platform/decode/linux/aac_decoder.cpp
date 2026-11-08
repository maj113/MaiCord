// The AAC decoder interface (include/maicord/platform/decode/aac_decoder.hpp) on Linux: the in-house AAC-LC core
// (src/media/audio/aac_lc.cpp) behind the calls the Media Foundation transform answers on Windows. One frame goes in
// per submit and its 1024 interleaved float samples come out of the next read, stamped with the packet's timestamp;
// the core has no delay of its own beyond the MDCT overlap, which the container's edit list accounts for as it does
// for the Windows decoder. HE-AAC (SBR, PS) is refused at initialization: its core stream is not the PCM the
// container's configuration promises.
#include "maicord/platform/decode/aac_decoder.hpp"
#include "../../audio/audio_support.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/media/audio/aac_lc.hpp"

namespace maicord {
namespace {
using Error = AacDecodeError;

constexpr u32 kFrameSamples = kAacLcFrameSamples;

struct State {
    AacLc* decoder;
    usize bytes;
    i64 pending_time;
    bool pending;
    float pcm[kFrameSamples * kAacLcMaximumChannels];
};

constexpr usize align_up(usize value) noexcept {
    return (value + kAacLcStateAlignment - 1) & ~(kAacLcStateAlignment - 1);
}

using audio_detail::disjoint;

// The decoder state and the State block in one allocation: the core first (it needs 64-byte alignment, a page gives
// it), then the block.
State* open(AacDecoder& owner) noexcept {
    const usize core = align_up(aac_lc_state_bytes()), total = core + align_up(sizeof(State));
    auto* memory = static_cast<u8*>(pages_allocate(total));
    if (!memory)
        return nullptr;
    AacLc* decoder = aac_lc_initialize(memory, owner.sample_rate, owner.channels);
    if (!decoder) {
        pages_release(memory, total);
        return nullptr;
    }
    auto* state = reinterpret_cast<State*>(memory + core);
    state->decoder = decoder;
    state->bytes = total;
    owner.runtime = memory;
    return state;
}

State* state_of(const AacDecoder& owner) noexcept {
    return owner.runtime ? reinterpret_cast<State*>(static_cast<u8*>(owner.runtime) +
                                                    align_up(aac_lc_state_bytes()))
                         : nullptr;
}
} // namespace

AacDecodeResult aac_decoder_initialize(AacDecoder* owner, ConstSlice<u8> asc, u32 rate, u32 channels) noexcept {
    if (!owner || !aac_decoder_configuration_valid(asc, rate, channels) ||
        !disjoint(owner, sizeof(*owner), asc.data, asc.count))
        return {Error::invalid_input};
    if (owner->initialized || owner->runtime)
        return {Error::busy};
    AacConfiguration configuration{};
    aac_configuration_decode(asc, &configuration);
    // SBR and PS need their own decoder; the AAC-LC core alone would give half-bandwidth audio at the wrong rate.
    if (configuration.sbr || configuration.ps || !aac_lc_supported(rate, channels))
        return {Error::unsupported};
    reset_value(owner);
    copy_bytes(owner->asc, asc.data, asc.count);
    owner->asc_bytes = asc.count;
    owner->sample_rate = rate;
    owner->channels = channels;
    owner->initialized = true;
    return {};
}

AacDecodeResult aac_decoder_submit(AacDecoder* owner, ConstSlice<u8> frame, i64 pts, u64 duration) noexcept {
    if (!owner || !owner->initialized || !frame.count || frame.count > kAacDecoderMaximumPacketBytes ||
        pts == (-0x7fffffffffffffffll - 1) || !duration || duration > static_cast<u64>(0x7fffffffffffffffll) ||
        !disjoint(owner, sizeof(*owner), frame.data, frame.count))
        return {Error::invalid_input};
    if (owner->draining || owner->ended)
        return {Error::end_of_stream};
    State* state = state_of(*owner);
    if (!state) {
        state = open(*owner);
        if (!state) {
            owner->native_status = -1;
            return {Error::runtime, owner->native_status};
        }
    }
    if (state->pending)
        return {Error::need_output};
    if (owner->accepted_frames == ~u64{0})
        return {Error::capacity};
    const AacLcError error = aac_lc_decode(state->decoder, frame, state->pcm);
    if (error != AacLcError::none) {
        // The core left its state as it was: the next packet decodes as if this one were missing.
        owner->native_status = static_cast<i32>(error);
        return {Error::input, owner->native_status};
    }
    state->pending = true;
    state->pending_time = pts;
    ++owner->accepted_frames;
    return {};
}

AacDecodedBlock aac_decoder_read(AacDecoder* owner, Slice<u8> pcm) noexcept {
    if (!owner || !owner->initialized || !disjoint(owner, sizeof(*owner), pcm.data, pcm.count))
        return {Error::invalid_input};
    if (owner->ended)
        return {Error::end_of_stream};
    if (pcm.count < kAacDecoderMaximumOutputBytes)
        return {Error::capacity};
    State* state = state_of(*owner);
    if (!state || !state->pending) {
        if (owner->draining) {
            owner->ended = true;
            return {Error::end_of_stream};
        }
        return {Error::need_input};
    }
    const u32 bytes = kFrameSamples * owner->channels * static_cast<u32>(sizeof(float));
    if (!disjoint(state, sizeof(*state), pcm.data, pcm.count))
        return {Error::invalid_input};
    copy_bytes(pcm.data, state->pcm, bytes);
    state->pending = false;
    owner->output_frames += kFrameSamples;
    return {Error::none, 0, bytes, kFrameSamples, state->pending_time, true};
}

AacDecodeResult aac_decoder_drain(AacDecoder* owner) noexcept {
    if (!owner || !owner->initialized)
        return {Error::invalid_input};
    if (owner->draining)
        return {};
    owner->draining = true;
    if (!owner->runtime)
        owner->ended = true;
    return {};
}

AacDecodeResult aac_decoder_flush(AacDecoder* owner) noexcept {
    if (!owner || !owner->initialized)
        return {Error::invalid_input};
    if (State* state = state_of(*owner)) {
        aac_lc_reset(state->decoder);
        state->pending = false;
    }
    owner->draining = owner->ended = false;
    owner->accepted_frames = owner->output_frames = 0;
    owner->native_status = 0;
    return {};
}

AacDecodeResult aac_decoder_release(AacDecoder* owner) noexcept {
    if (!owner)
        return {Error::invalid_input};
    if (State* state = state_of(*owner)) {
        void* memory = owner->runtime;
        const usize bytes = state->bytes;
        if (!pages_release(memory, bytes))
            return {Error::release, -1};
    }
    reset_value(owner);
    return {};
}

} // namespace maicord
