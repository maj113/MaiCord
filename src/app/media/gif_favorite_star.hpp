#pragma once

namespace maicord::app_detail {
// The favourite star on GIFs in the message list (.gif attachments and image
// embeds, and looping GIFV embeds). It shows while the GIF is hovered.

// Publishes the hovered GIF's favourite state to the renderer, loading the
// saved favourites when a GIF is first hovered. Once per frame.
void application_gif_favorite_star_synchronize() noexcept;
// Pure pointer query for cursor resolution: true over a GIF's star.
bool application_gif_favorite_star_target(float x, float y) noexcept;
// Primary-button press: on a GIF's star, saves or removes the GIF and
// consumes the press. Only WM_LBUTTONDOWN routes may call it.
bool application_gif_favorite_star_press(float x, float y) noexcept;
} // namespace maicord::app_detail
