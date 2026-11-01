// Translate: translates messages with Google's free endpoint or DeepL, shows
// the result in a card under the message, and translates text for you to send.
#include <maicord/sdk/plugin.hpp>
using namespace mc;

MC_SETTINGS((Choice, target, "Translate to", 0, .items = {"English", "German", "French", "Japanese", "Spanish"}),
            (Toggle, outgoing, "Ask before sending untranslated messages", false),
            (Choice, service, "Service", 0, .items = {"Google (free)", "DeepL"}),
            (Secret, deepl_key, "DeepL API key", {}, .visible_if = Setting::service, .visible_value = 1));

constexpr Str kGoogleLanguages[] = {"en", "de", "fr", "ja", "es"};
constexpr Str kDeeplLanguages[] = {"EN", "DE", "FR", "JA", "ES"};

// The last translations, oldest replaced first.
struct State {
    struct Entry {
        u64 message;
        Text<MC_CARD_TEXT_MAX> text;
    } done[8];
    u32 next;
};
MC_STATE(State, .version = 1);

static State::Entry* find(Ctx& ctx, u64 message) {
    for (State::Entry& entry : ctx.state<State>().done)
        if (entry.message == message && message)
            return &entry;
    return nullptr;
}

// -- Requests ------------------------------------------------------------------

static bool deepl(Ctx& ctx) {
    return ctx.settings().service == 1 && !ctx.settings().deepl_key.empty();
}
template <auto Done> static mc_status request(Ctx& ctx, Str text, u64 tag) {
    const u32 language = ctx.settings().target;
    Text<MC_HTTP_BODY_MAX> body;
    if (deepl(ctx)) {
        // Free-plan keys end in ":fx" and use their own host.
        const Str key = ctx.settings().deepl_key;
        const bool free = key.bytes > 3 && key.sub(key.bytes - 3) == Str(":fx");
        body.append("target_lang=");
        body.append(kDeeplLanguages[language]);
        body.append("&text=");
        if (!url::encode(text, body))
            return MC_E_CAPACITY;
        Text<MC_HTTP_HEADER_VALUE_MAX> authorization;
        authorization.append("DeepL-Auth-Key ");
        authorization.append(key);
        return ctx.http.post<Done>(free ? Str("https://api-free.deepl.com/v2/translate")
                                        : Str("https://api.deepl.com/v2/translate"),
                                   body, tag,
                                   {{"Content-Type", "application/x-www-form-urlencoded"},
                                    {"Authorization", authorization}});
    }
    Text<128> address;
    address.append("https://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&dt=t&tl=");
    address.append(kGoogleLanguages[language]);
    body.append("q=");
    if (!url::encode(text, body))
        return MC_E_CAPACITY;
    return ctx.http.post<Done>(address, body, tag, {{"Content-Type", "application/x-www-form-urlencoded"}});
}

// Google answers [[["translated","source",...],["more","...",...],...],...]:
// the translation is the first string of each segment.
static bool google_text(Str body, Text<MC_CARD_TEXT_MAX>& out) {
    u32 at = 0;
    json::skip_space(body, at);
    if (!body.sub(at).starts_with("[[["))
        return false;
    at += 2;
    while (at < body.bytes && body[at] == '[') {
        ++at;
        Text<MC_CARD_TEXT_MAX> piece;
        if (!json::read_string(body, at, piece) || !out.append(piece))
            return false;
        while (at < body.bytes && body[at] != ']') // the rest of the segment
            if (body[at] == ',' || json::space(body[at]))
                ++at;
            else if (!json::skip(body, at))
                return false;
        ++at;
        json::skip_space(body, at);
        if (at < body.bytes && body[at] == ',')
            ++at;
    }
    return !out.empty();
}
static bool translation(Ctx& ctx, const HttpResult& r, Text<MC_CARD_TEXT_MAX>& out) {
    return r.ok() && (deepl(ctx) ? json::find(r.body(), "text", out) : google_text(r.body(), out));
}

// -- A message's translation -----------------------------------------------------

static void on_translated(Ctx& ctx, const HttpResult& r, u64 message) {
    State& state = ctx.state<State>();
    State::Entry& entry = state.done[state.next++ % 8];
    entry.message = 0;
    if (!translation(ctx, r, entry.text)) {
        ctx.toast(r.ok() ? Str("Unexpected answer from the translation service") : Str("Translation failed"),
                  Tone::critical);
        return;
    }
    entry.message = message;
    ctx.invalidate_message(message); // the decorator below is asked again
}

static bool has_text(Ctx&, const ButtonEvent& button) {
    return !button.message().content().empty();
}
MC_BUTTON(translate, surface::message_popover, "Translate", "lucide/languages", .visible = has_text) {
    if (request<on_translated>(ctx, button.message().content(), button.message().id()) != MC_OK)
        ctx.toast("Couldn't start the translation", Tone::warning);
}

static void decorate(Ctx& ctx, const Message& m, Decoration& d) {
    if (const State::Entry* entry = find(ctx, m.id()))
        d.card(card().text(entry->text).caption("Translated").button("Hide", m.id()));
}
static void on_hide(Ctx& ctx, const NodeAction& action) {
    if (State::Entry* entry = find(ctx, action.tag)) {
        entry->message = 0;
        ctx.invalidate_message(action.tag);
    }
}

// -- Your messages ----------------------------------------------------------------

static void pre_send(Ctx& ctx, Draft& draft) {
    if (ctx.settings().outgoing)
        draft.confirm("Send this untranslated? /tr translates text into the composer first.");
}

static void on_command_done(Ctx& ctx, const HttpResult& r, u64) {
    Text<MC_CARD_TEXT_MAX> text;
    if (translation(ctx, r, text))
        ctx.composer.insert(text); // the command's gesture carries over to its completion
    else
        ctx.toast("Translation failed", Tone::critical);
}
MC_COMMAND(tr, (Str text), "Translate text and put it in the composer", .args = {"The text to translate"}) {
    return request<on_command_done>(ctx, text, 0) == MC_OK ? Reply::none()
                                                           : Reply::toast("Text too long", Tone::warning);
}

MC_HOOKS(decorate, on_hide, pre_send);
MC_PLUGIN(.owner = 0x6D63'7472'616E'736C, .name = "Translate", .version = "1.0.0",
          .summary = "Translate messages with Google or DeepL.", .permissions = MC_PERM_COMPOSER,
          .network = {"translate.googleapis.com", "api-free.deepl.com", "api.deepl.com"});
