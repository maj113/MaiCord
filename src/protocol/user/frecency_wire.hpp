#pragma once

#include "../encoding/proto_wire.hpp"
#include "maicord/core/frecency.hpp"

namespace maicord::protocol_detail {

// The frecency maps of settings-proto/2 (commands, emoji, reactions, stickers): map<string, FrecencyItem> with
// key = 1 and value = 2; an item has total_uses = 1, recent_uses = 2 (packed or not), frecency = 3 and score = 4.
enum class FrecencyWire : u8 { ok, protobuf, utf8 };

// One map entry in [p, stop) of a raw source. A missing key leaves key.data null; a missing value is a default item.
// Recent uses keep the newest ten, ascending; unknown fields are skipped.
FrecencyWire frecency_entry_decode(const ProtoSource& w, u32 p, u32 stop, Utf8View* key, FrecencyItem* item) noexcept;
// One entry as a repeated field 1 of the map: key and item, zero fields omitted.
void frecency_entry_write(ProtoWriter& out, const Utf8View& key, const FrecencyItem& item) noexcept;
// The data version (field 3) of the settings proto's versions group [p, end); other fields are skipped.
bool settings_data_version(const ProtoSource& w, u32 p, u32 end, u32* version) noexcept;

// A length-delimited group of `field`: the body runs twice, once to size it.
template <class Body> void frecency_group_write(ProtoWriter& out, u32 field, Body body) noexcept {
    ProtoWriter counter{};
    body(counter);
    out.tag(field, 2);
    out.varint(counter.at);
    body(out);
}

} // namespace maicord::protocol_detail
