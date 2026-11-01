#pragma once
// Copying take of a CDN response for the live probe and the transport benchmark. The client's shipping
// take is the in-place consumer; this copies the response and body out through it.
#include "maicord/app/net/cdn_client.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace cdn_take_detail {
struct Target {
    CdnClientResponse* response;
    Slice<u8> body;
};
inline bool copy_out(void* context, const CdnClientResponse& response, ConstSlice<u8> body) noexcept {
    auto& target = *static_cast<Target*>(context);
    if (body.count > target.body.count)
        return false;
    copy_bytes(target.body.data, body.data, body.count);
    copy_value(target.response, response);
    return true;
}
} // namespace cdn_take_detail

inline CdnClientTakeResult cdn_client_take_response(CdnClient* client, CdnClientResponse* response,
                                                    Slice<u8> body_output) noexcept {
    if (!response || (!body_output.data && body_output.count != 0))
        return {0, CdnClientTakeError::invalid_input};
    cdn_take_detail::Target target{response, body_output};
    return cdn_client_consume_response(client, cdn_take_detail::copy_out, &target);
}
} // namespace maicord
