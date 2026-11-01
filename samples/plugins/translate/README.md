# Translate

Translates messages with Google's free endpoint or DeepL. A **Translate**
button on a message's hover toolbar sends its text to the chosen service and
shows the result in a card under the message, with a **Hide** button. `/tr
text` translates text and puts it in the composer. With "Ask before sending
untranslated messages" on, sending asks first.

| Setting | Type | Default |
|---|---|---|
| Translate to (`target`) | Choice: English, German, French, Japanese, Spanish | English |
| Ask before sending untranslated messages (`outgoing`) | Toggle | off |
| Service (`service`) | Choice: Google (free), DeepL | Google |
| DeepL API key (`deepl_key`) | Secret, shown for DeepL only | empty |

Permissions: `composer` (declared, for `/tr`); `network`, `read_messages` and
`message_send`, implied by its hosts, its popover button, its decoration and
its `pre_send` hook. Hosts: `translate.googleapis.com`, `api-free.deepl.com`,
`api.deepl.com`. The DeepL key is sent only to DeepL; free-plan keys (ending
in `:fx`) use `api-free.deepl.com`.

It shows the SDK's network, button, card, command, secret-setting and state
features in about 150 lines; build it like `samples/plugins/clear-urls`.
