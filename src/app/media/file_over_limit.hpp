#pragma once
#include "maicord/app/chat/composer/message_attachment_drafts.hpp"
#include "maicord/ui/chat/composer/attachment_strip.hpp"

namespace maicord::app_detail {
// "<file name> is 27.3 MB, over the 10 MB limit." for the picked file at `path` (its last component names it).
// The sentence is written to `storage`, which the result views; the picked file's size is the one its check already
// measured.
inline Utf8View application_file_over_limit_text(ConstSlice<wchar_t> path, u64 bytes, u64 limit,
                                                 Slice<char> storage) noexcept {
    char name[kAttachmentUploadMaximumFilenameBytes];
    const u32 name_bytes = message_attachment_file_name({path, {}, {}, {}}, {name, sizeof(name)});
    return {storage.data, ui_attachment_over_limit_text({name, name_bytes}, bytes, limit, 0, storage)};
}
} // namespace maicord::app_detail
