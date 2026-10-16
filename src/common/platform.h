#pragma once

#include <cstdint>
#include <string>

namespace strojanka {

std::string executable_path();  // absolute path of the running program
std::string executable_dir();
void enable_utf8_console();     // Windows: make the console print UTF-8 correctly
int64_t system_boot_unix();     // when the computer last started (unix seconds), 0 if unknown

}  // namespace strojanka
