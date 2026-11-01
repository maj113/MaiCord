// Reverse Image Search: a "Search image" submenu on images with one item per engine.
#include <maicord/sdk/plugin.hpp>
using namespace mc;

MC_SETTINGS((Toggle, tineye, "Show TinEye", true));

// Opens engine + the percent-encoded image URL (the host confirms external links).
static void search(Ctx& ctx, const MenuTarget& target, Str engine) {
    Text<MC_HTTP_URL_MAX> address;
    address.append(engine);
    if (url::encode(target.url(), address))
        ctx.open_url(address);
    else
        ctx.toast("This image's address is too long to search for", Tone::warning);
}
static bool has_url(Ctx&, const MenuTarget& target) {
    return !target.url().empty();
}
static bool tineye_shown(Ctx& ctx, const MenuTarget&) {
    return ctx.settings().tineye;
}

MC_SUBMENU(engines, menu::image, "Search image", .visible = has_url);
MC_MENU(google, menu::image, "Google Lens", .parent = engines) {
    search(ctx, target, "https://lens.google.com/uploadbyurl?url=");
}
MC_MENU(bing, menu::image, "Bing", .parent = engines) {
    search(ctx, target, "https://www.bing.com/images/search?view=detailv2&iss=sbi&q=imgurl:");
}
MC_MENU(yandex, menu::image, "Yandex", .parent = engines) {
    search(ctx, target, "https://yandex.com/images/search?rpt=imageview&url=");
}
MC_MENU(tineye, menu::image, "TinEye", .parent = engines, .visible = tineye_shown) {
    search(ctx, target, "https://www.tineye.com/search?url=");
}

MC_PLUGIN(.owner = 0x6D63'7269'6D67'7372, .name = "Reverse Image Search", .version = "1.0.0",
          .summary = "Searches the web for an image from its context menu.");
