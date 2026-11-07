#pragma once
#include "platform/Platform.h"
#include <oleidl.h>

namespace app {
// Caller owns one COM reference; RegisterDragDrop retains its own reference.
IDropTarget *create_file_drop_target(std::function<bool()> enabled,
    std::function<void(bool)> hover,
    std::function<void(std::vector<std::filesystem::path>)> drop);
}
