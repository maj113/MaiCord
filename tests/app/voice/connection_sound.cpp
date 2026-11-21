#include "maicord/app/voice/connection_sound.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
using S = VoiceConnectionSound;
using P = UiVoicePanelState;
constexpr u64 kChannel = 41, kOther = 42, kServer = 7, kNewServer = 8;

struct Look {
    u64 channel, generation;
    P state;
    S sound;
    bool stage = false;
};
// Feeds the looks to a new tracker in order and compares each sound.
template <u32 N> bool plays(const Look (&looks)[N]) noexcept {
    VoiceConnectionSoundTracker tracker{};
    for (const auto& look : looks)
        if (voice_connection_sound_step(&tracker, look.channel, look.generation, look.stage, look.state) != look.sound)
            return false;
    return true;
}
} // namespace

void test_voice_connection_sound(Check check) noexcept {
    check(static_cast<u8>(S::join) == 3 && static_cast<u8>(S::leave) == 4 && static_cast<u8>(S::interrupted) == 5,
          "Voice connection sounds: join, leave and interrupted are the bank's clips 3, 4 and 5");
    check(plays({{0, 0, P::hidden, S::none},
                 {kChannel, kServer, P::connecting, S::none},
                 {kChannel, kServer, P::connected, S::join},
                 {kChannel, kServer, P::connected, S::none},
                 {0, 0, P::hidden, S::leave},
                 {0, 0, P::hidden, S::none}}),
          "Voice connection sounds: a call plays one join when it connects and one leave when it ends");
    check(plays({{kChannel, kServer, P::connected, S::join},
                 {kChannel, kServer, P::connecting, S::interrupted},
                 {kChannel, kServer, P::connecting, S::none},
                 {kChannel, kServer, P::connected, S::join},
                 {kChannel, kServer, P::failed, S::interrupted},
                 {kChannel, kServer, P::failed, S::none},
                 {0, 0, P::hidden, S::none}}),
          "Voice connection sounds: a lost connection plays interrupted once, a reconnect plays join, and a "
          "failure plays interrupted");
    check(plays({{kChannel, kServer, P::connected, S::join},
                 {kOther, kNewServer, P::connecting, S::leave},
                 {kOther, kNewServer, P::connected, S::join},
                 {kOther, kNewServer + 1, P::connecting, S::leave},
                 {kOther, kNewServer + 1, P::connected, S::join},
                 {kChannel, kServer, P::connected, S::join}}),
          "Voice connection sounds: a move to another channel or voice server plays leave and join, never "
          "interrupted");
    check(plays({{kChannel, kServer, P::connecting, S::none}, {0, 0, P::hidden, S::none}}),
          "Voice connection sounds: a join cancelled before it connects is silent");
    check(plays({{kChannel, kServer, P::connecting, S::none, true},
                 {kChannel, kServer, P::connected, S::none, true},
                 {kChannel, kServer, P::connecting, S::none, true},
                 {kChannel, kServer, P::connected, S::none, true},
                 {0, 0, P::hidden, S::none}}),
          "Voice connection sounds: a stage channel plays no connection sound");
}
} // namespace maicord
