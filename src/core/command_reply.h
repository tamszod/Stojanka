#pragma once

#include <string>

namespace strojanka {

struct CommandReply {
    bool ok = true;
    std::string text;
};

}  // namespace strojanka
