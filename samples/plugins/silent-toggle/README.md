# Silent Toggle

A chat-bar button and a `/silent` command that send your messages as
`@silent` (no push notifications) while it's on. `/silent` switches it,
`/silent on:true` or `on:false` sets it.

| Setting | Type | Default |
|---|---|---|
| Send messages silently (`silent`) | Toggle, hidden; the button and command change it | off |
| Turn off after one message (`once`) | Toggle | off |

Permissions: `message_send`, implied by its `pre_send` hook. It shows
`MC_BUTTON` with `.toggle` and `ctx.button<>()` state, `MC_COMMAND` with an
optional argument and `Reply::toast`, and `ctx.set_setting`. Build it like
`samples/plugins/clear-urls`.
