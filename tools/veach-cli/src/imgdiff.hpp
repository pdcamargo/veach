#pragma once

#include <string_view>
#include <vector>

/// `veach imgdiff <reference.png> <test.png> [--max-mean F] [--diff out.png]`. Returns the exit code.
int command_imgdiff(const std::vector<std::string_view>& args);
