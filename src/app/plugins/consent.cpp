#include "maicord/app/plugins/consent.hpp"

namespace maicord {

PluginConsentRequest plugin_consent_request(const PluginImage& image, Utf8View (&hosts)[MC_NETWORK_HOSTS_MAX]) noexcept {
    const mc_descriptor& d = *image.descriptor;
    for (u32 i = 0; i < d.network_count; ++i) hosts[i] = {d.network[i].data, d.network[i].bytes};
    return {{image.info.name, image.info.name_bytes},
            {image.info.version, image.info.version_bytes},
            {image.info.author, image.info.author_bytes},
            image.permissions,
            {hosts, d.network_count},
            0,
            0};
}

} // namespace maicord
