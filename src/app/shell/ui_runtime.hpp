#pragma once

namespace maicord::app_detail {

struct Application;

bool application_initialize_shell_scenes(Application& application) noexcept;
bool application_initialize_message_scene(Application& application) noexcept;
void application_release_widget_storage(Application& application) noexcept;

bool application_initialize_account_ui(Application& application) noexcept;
void application_release_account_ui(Application& application) noexcept;

} // namespace maicord::app_detail
