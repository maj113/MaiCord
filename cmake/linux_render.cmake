# The shared renderer of the Linux build (src/render): the code the Windows build compiles over Direct3D 11, here over
# the OpenGL ES backend of cmake/linux_gpu.cmake through the same gpu_* API, with the UI scenes it paints.

# The packed UI data the scenes embed (the Windows build's cmake/generated_sources.cmake rules, minus the shaders and the
# icon): the emoji catalog and pools, and the icon pack.
set(MAICORD_EMOJI_CATALOG_INCLUDE "${MAICORD_GENERATED_DIRECTORY}/unicode_emoji_16_optimized.inc")
add_custom_command(
    OUTPUT "${MAICORD_EMOJI_CATALOG_INCLUDE}"
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_unicode_emoji_catalog.py"
        --canonical-include
        --shortcode-include "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/text/emoji_shortcodes.inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/text/unicode_emoji_16.inc"
        "${MAICORD_EMOJI_CATALOG_INCLUDE}"
    DEPENDS tools/generate_unicode_emoji_catalog.py src/ui/text/unicode_emoji_16.inc src/ui/text/emoji_shortcodes.inc
    VERBATIM)
set(MAICORD_EMOJI_POOL_INCLUDE "${MAICORD_GENERATED_DIRECTORY}/emoji_pool_pack.inc")
set(MAICORD_EMOJI_POOL_PACKED "${MAICORD_GENERATED_DIRECTORY}/emoji_pool_packed.bin")
add_custom_command(
    OUTPUT "${MAICORD_EMOJI_POOL_INCLUDE}" "${MAICORD_EMOJI_POOL_PACKED}"
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_emoji_pools.py" "${MAICORD_EMOJI_POOL_INCLUDE}"
        "${MAICORD_EMOJI_CATALOG_INCLUDE}" "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/text/emoji_shortcodes.inc"
    DEPENDS tools/pack_emoji_pools.py tools/generate_unicode_emoji_catalog.py tools/font_reference/embed_fixture.py
        tools/optimal_deflate.py tools/deflate_cache.txt "${MAICORD_EMOJI_CATALOG_INCLUDE}" src/ui/text/emoji_shortcodes.inc
    VERBATIM)
add_custom_target(maicord_emoji_catalog_data
    DEPENDS "${MAICORD_EMOJI_CATALOG_INCLUDE}" "${MAICORD_EMOJI_POOL_INCLUDE}")

file(GLOB MAICORD_ICON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lucide/icons/*.svg"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/original/*.svg")
set(MAICORD_ICON_PACK_INCLUDE "${MAICORD_GENERATED_DIRECTORY}/ui_icon_pack.inc")
set(MAICORD_ICON_PACK_DATA "${MAICORD_GENERATED_DIRECTORY}/ui_icon_pack_data.bin")
set(MAICORD_ICON_PACK_PACKED "${MAICORD_GENERATED_DIRECTORY}/ui_icon_pack_packed.bin")
add_custom_command(
    OUTPUT "${MAICORD_ICON_PACK_INCLUDE}" "${MAICORD_ICON_PACK_DATA}" "${MAICORD_ICON_PACK_PACKED}"
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_icon_pack.py" build
        --manifest "${CMAKE_CURRENT_SOURCE_DIR}/include/maicord/ui/widgets/icon_ids.inc"
        --lucide "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lucide/icons"
        --original "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/original"
        --output "${MAICORD_ICON_PACK_INCLUDE}"
    DEPENDS tools/generate_icon_pack.py tools/optimal_deflate.py tools/deflate_cache.txt tools/font_reference/embed_fixture.py
        include/maicord/ui/widgets/icon_ids.inc
        ${MAICORD_ICON_SOURCES}
    VERBATIM)
add_custom_target(maicord_icon_pack_data DEPENDS "${MAICORD_ICON_PACK_INCLUDE}")

maicord_library(maicord_render_core
    src/render/media/animation_frame_cache.cpp src/render/text/glyph_atlas.cpp src/render/text/glyph_quad_bounds.cpp
    src/render/text/glyph_raster.cpp src/render/media/atlas.cpp src/render/d3d11/presentation_damage.cpp
    LIBS maicord_core)

maicord_library(maicord_atlas_gpu
    src/render/text/glyph_atlas_gpu.cpp
    src/render/media/atlas_gpu.cpp src/render/media/atlas_gpu_animation.cpp
    src/render/media/atlas_gpu_dispatch.cpp src/render/media/atlas_gpu_operations.cpp
    src/render/media/atlas_gpu_validation.cpp src/render/media/atlas_gpu_vp8.cpp
    LIBS maicord_core maicord_render_core maicord_gpu_gles
    INCLUDES src "${MAICORD_GENERATED_DIRECTORY}")

# The geometry the Lottie compiler (Direct2D's job on Windows) runs on here: flattening, boolean combination, area and
# stroke widening of paths.
maicord_library(maicord_vector_geometry
    src/render/effects/vector_geometry.cpp src/render/effects/vector_overlay.cpp src/render/effects/vector_stroke.cpp
    LIBS maicord_core)

# Colour glyph paint programs: the COLR, regional flag and Lottie programs the glyph atlas rasterizes.
maicord_library(maicord_paint_program
    src/render/effects/colr_program.cpp src/render/effects/colr_source.cpp src/render/effects/regional_flag_program.cpp
    src/render/effects/lottie_program.cpp
    LIBS maicord_vector_geometry maicord_core maicord_render_core maicord_font maicord_media_core)

maicord_library(maicord_theme src/ui/theme/theme.cpp LIBS maicord_core)

# The scenes the renderer paints and routes input over. The text system entry points, the emoji presentation and the
# theme typography are maicord_text_system's here.
maicord_library(maicord_ui
    src/ui/chat/polls/controls.cpp src/ui/chat/polls/voters.cpp src/ui/chat/polls/presentation.cpp src/ui/voice/audio_controls.cpp
    src/ui/voice/audio_presentation.cpp src/ui/voice/video_controls.cpp src/ui/voice/video_scene.cpp src/ui/widgets/spinner.cpp
    src/ui/core/transition.cpp src/ui/widgets/tooltip.cpp src/ui/text/ansi_text.cpp
    src/ui/text/code_highlight.cpp src/ui/chat/composer/autocomplete.cpp src/ui/chat/messages/markdown.cpp
    src/ui/chat/messages/markdown_links.cpp
    src/ui/text/timestamp_format.cpp src/ui/shell/navigation_view.cpp src/ui/text/emoji_catalog.cpp src/ui/core/geometry.cpp
    src/ui/guild/rail_scene.cpp src/ui/shell/navigation_drag.cpp src/ui/chat/unread_banner_scene.cpp
    src/app/chat/unread_banner_projection.cpp src/ui/layout/layout_engine.cpp src/ui/account/login_scene.cpp
    src/ui/account/login_presentation.cpp src/ui/account/login_text_presentation.cpp src/ui/account/login_view.cpp
    src/ui/guild/member_list_scene.cpp src/ui/guild/member_list_navigation.cpp src/ui/chat/messages/action_bar.cpp
    src/ui/chat/messages/action_bar_presentation.cpp src/ui/chat/messages/context_controller.cpp
    src/ui/chat/messages/context_menu.cpp src/ui/account/user_context_menu.cpp src/ui/chat/unread_context.cpp
    src/ui/chat/messages/rich_layout.cpp src/ui/chat/messages/component_layout.cpp src/ui/chat/messages/component_media.cpp
    src/ui/chat/messages/media_policy.cpp src/ui/chat/messages/system_text.cpp src/ui/chat/messages/rich_widgets.cpp
    src/ui/chat/messages/scene.cpp src/ui/chat/messages/scene_rows.cpp src/ui/chat/messages/scene_navigation.cpp
    src/ui/chat/messages/scene_measurements.cpp
    src/ui/layout/scrollbar.cpp src/ui/chat/composer/message_composer_scene.cpp src/ui/chat/composer/attachment_strip.cpp
    src/ui/chat/composer/voice_bar.cpp src/ui/chat/messages/presentation.cpp src/ui/media/image_viewer.cpp
    src/ui/media/zoom_lens.cpp
    src/ui/chat/messages/reaction_presentation.cpp src/ui/chat/messages/typing_presentation.cpp src/ui/core/paint.cpp
    src/ui/widgets/icon.cpp src/ui/widgets/button.cpp src/ui/chat/messages/text_coordinator.cpp src/ui/chat/messages/text_presentation.cpp
    src/ui/chat/messages/text_presentation_reply.cpp src/ui/chat/messages/text_presentation_content.cpp
    src/ui/chat/messages/text_presentation_cards.cpp src/ui/chat/messages/text_presentation_row.cpp
    src/ui/shell/popup_scene.cpp src/ui/guild/channel_context_policy.cpp src/ui/guild/context_policy.cpp
    src/ui/guild/member_context_policy.cpp src/ui/chat/composer/attachment_menu.cpp src/ui/chat/messages/received_attachment_menu.cpp
    src/ui/shell/popup_presentation.cpp src/ui/text/edit_menu.cpp src/ui/text/spell_tokenize.cpp
    src/ui/chat/composer/spellcheck.cpp src/ui/text/form.cpp src/ui/text/composition.cpp src/ui/widgets/qr_code.cpp
    src/ui/chat/messages/reaction_picker.cpp src/ui/text/edit_field.cpp src/ui/widgets/badge.cpp src/ui/account/profile_scene.cpp
    src/ui/account/profile_scene_sections.cpp src/ui/account/profile_scene_layout.cpp
    src/ui/account/profile_media.cpp src/ui/account/profile_dialog_scene.cpp src/ui/chat/messages/reaction_viewer.cpp
    src/ui/chat/messages/reaction_viewer_presentation.cpp src/ui/core/scene.cpp src/ui/kit/scene_kit.cpp src/ui/shell/navigation_scene.cpp
    src/ui/shell/presentation.cpp src/ui/shell/text_presentation.cpp src/ui/chat/private_channel_title.cpp
    src/ui/chat/private_channel_rows.cpp src/ui/layout/smooth_scroll.cpp src/ui/layout/scroll_host.cpp src/ui/text/flow_layout.cpp
    src/ui/text/measure_cache.cpp src/ui/text/presentation.cpp src/ui/layout/virtual_list.cpp src/ui/core/widget_manager.cpp
    src/ui/core/widget_builder.cpp src/ui/widgets/menu_row.cpp src/ui/widgets/button_keyboard.cpp src/ui/core/input_router.cpp
    src/ui/layout/list_navigation.cpp src/ui/layout/list_paging.cpp src/ui/layout/widget_scroll.cpp src/ui/core/widget_storage.cpp
    src/ui/shell/page_registry.cpp src/ui/settings/scene.cpp src/ui/settings/scene_layout.cpp
    src/ui/settings/scene_paint.cpp src/ui/settings/theme_editor.cpp
    src/ui/theme/style_projection.cpp src/ui/widgets/settings_choice.cpp src/ui/account/friends_scene.cpp
    src/ui/widgets/kit_fields.cpp src/ui/widgets/kit_collections.cpp
    src/ui/widgets/kit_markdown.cpp
    src/ui/store/store_layout.cpp src/ui/store/store_input.cpp src/ui/store/store_paint.cpp src/ui/store/store_access.cpp
    src/ui/account/friends_media.cpp src/ui/controls/settings_tokens.cpp src/ui/controls/switch.cpp
    src/ui/controls/radio.cpp src/ui/controls/slider.cpp src/ui/controls/input.cpp src/ui/controls/swatch.cpp
    src/ui/controls/navigation.cpp src/ui/controls/text_metrics.cpp src/ui/settings/schema.cpp
    src/ui/controls/permission_grid.cpp src/ui/controls/admin_list.cpp src/ui/accessibility/accessibility.cpp
    src/ui/accessibility/scenes.cpp src/ui/widgets/icon_pack.cpp src/ui/account/menu.cpp src/ui/account/switcher.cpp
    src/ui/chat/search/scene.cpp src/ui/chat/search/scene_items.cpp src/ui/chat/search/scene_paint.cpp
    src/ui/chat/pinned_messages_scene.cpp src/ui/chat/inbox_scene.cpp src/ui/chat/threads/header.cpp
    src/ui/chat/threads/browser_scene.cpp src/ui/chat/threads/forum_channel_scene.cpp src/ui/chat/threads/forum_post_scene.cpp
    src/ui/chat/composer/attachment_draft_list.cpp src/ui/shell/quick_switcher_scene.cpp src/ui/chat/messages/forward_scene.cpp
    src/ui/media/download_scene.cpp src/ui/shell/shortcuts_scene.cpp src/ui/media/picker.cpp
    src/ui/media/picker_layout.cpp
    src/ui/media/picker_presentation.cpp src/ui/chat/search/query.cpp src/ui/chat/search/filter_context.cpp
    src/ui/voice/channel_rows.cpp src/ui/voice/panel.cpp
    src/ui/voice/call_scene.cpp src/ui/voice/call_stage.cpp src/ui/widgets/hover_card.cpp src/ui/voice/stream_card_scene.cpp
    src/ui/guild/card_scene.cpp src/ui/guild/folder_open.cpp src/ui/guild/hidden_channel_menu.cpp
    src/ui/guild/banner.cpp src/ui/account/profile_activity.cpp src/ui/shell/caption_bar.cpp src/ui/widgets/activity_status.cpp
    src/ui/chat/messages/typing_indicator.cpp src/ui/account/dm_profile_panel.cpp
    src/ui/widgets/toast.cpp src/ui/theme/background_color_math.cpp src/ui/chat/search/message_result_card.cpp
    src/ui/text/pointer_selection.cpp src/ui/chat/messages/author.cpp src/ui/chat/messages/interaction.cpp
    src/ui/media/expression_card_scene.cpp
    LIBS maicord_core maicord_model maicord_media_core maicord_unicode maicord_compression maicord_media_codec maicord_theme
    INCLUDES src "${MAICORD_GENERATED_DIRECTORY}"
    DEPENDS maicord_emoji_catalog_data maicord_icon_pack_data)
target_compile_options(maicord_ui PRIVATE -Wno-c23-extensions)

maicord_library(maicord_renderer
    src/render/media/animation_frame_cache_gpu.cpp src/render/text/content_text_retention.cpp
    src/render/text/shaped_text_cache.cpp src/render/d3d11/device.cpp src/render/d3d11/draw_pipeline.cpp
    src/render/d3d11/glyph_text.cpp src/render/media/icon_atlas.cpp
    src/render/text/native_glyph_outlines.cpp src/render/d3d11/background_fx.cpp
    src/render/d3d11/effect_glyphs.cpp
    src/render/d3d11/renderer.cpp src/render/d3d11/renderer_draw.cpp src/render/d3d11/renderer_edit.cpp
    src/render/d3d11/renderer_layout.cpp src/render/d3d11/renderer_layout_media.cpp src/render/d3d11/renderer_text.cpp
    src/render/d3d11/renderer_text_content.cpp src/render/d3d11/renderer_text_overlay.cpp
    src/render/d3d11/renderer_interaction.cpp src/render/d3d11/renderer_media.cpp
    src/render/d3d11/navigation.cpp src/render/text/edit_layout.cpp src/render/text/form_surface.cpp
    src/render/text/forum_post_surface.cpp src/render/text/message_hit_test.cpp src/render/d3d11/renderer_presentation.cpp
    src/render/d3d11/layer_presentation.cpp src/render/media/renderer_media_draw_storage.cpp
    src/render/d3d11/renderer_caption.cpp
    LIBS maicord_media_core maicord_font maicord_gpu_gles maicord_core maicord_atlas_gpu maicord_effect_host
    INCLUDES src "${MAICORD_GENERATED_DIRECTORY}")
