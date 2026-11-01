# Reverse Image Search

Adds a **Search image** submenu to the context menu of images, with Google
Lens, Bing, Yandex and TinEye. Each item opens the engine with the image's
address; MaiCord asks before opening the external link.

| Setting | Type | Default |
|---|---|---|
| Show TinEye (`tineye`) | Toggle | on |

Permissions: none. It shows `MC_SUBMENU`, `MC_MENU` with `.parent` and
`.visible`, `url::encode` and `ctx.open_url`. Build it like
`samples/plugins/clear-urls`.
