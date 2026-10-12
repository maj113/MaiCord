#pragma once
#include "application_composer_text_input.hpp"
#include "maicord/ui/secure_text_store.hpp"
namespace maicord::app_detail {
struct ApplicationComposerTextInput {
    SecureTextStore store;
    SecureTextComposition composition;
    TextInputTarget target;
    u32 observed_text_revision;
    u32 held_key;
    SecureTextStoreGeometry observed_geometry;
    u32 observed_layout_revision;
    u32 observed_layout_builds;
    bool geometry_valid;
    bool busy;
    bool dirty;
    bool failed;
};
}
