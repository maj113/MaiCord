#pragma once
#include "maicord/model/upload_allowance.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/json_scan.hpp"
namespace maicord {
// Server-assigned Apex user experiments, scoped to this authenticated user.
bool upload_policy_decode(JsonValue apex, u64 user_id, Slice<u32> scratch, UploadPolicy* policy) noexcept;
bool upload_policy_state(JsonValue state, u64 user_id, Slice<u32> scratch, UploadPolicy* policy, bool* changed) noexcept;
}
