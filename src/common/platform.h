#pragma once

#include <string>

namespace strojanka {

std::string executable_path();  // absolute path of the running program
std::string executable_dir();
void enable_utf8_console();     // Windows: make the console print UTF-8 correctly

}  // namespace strojanka
