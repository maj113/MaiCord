# Show Me Your Name

Shows each person's username beside the display name or server nickname on
message author lines and, optionally, member-list rows.

| Setting | Type | Default |
|---|---|---|
| Username goes (`place`) | Choice: after the name, before the name (in the role colour) | after |
| Also in the member list (`in_members`) | Toggle | on |
| Skip names that already match (`skip_same`) | Toggle | on |

Permissions: none. It's one `DECORATE_USER` hook filtered to message authors
and member rows with `mc::filter`. Build it like `samples/plugins/clear-urls`.
