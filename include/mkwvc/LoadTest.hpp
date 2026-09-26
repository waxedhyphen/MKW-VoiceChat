#pragma once

#include <span>
#include <string>

namespace mkwvc {

bool loadTestRequested(std::span<const std::string> args);
int runLoadTest(std::span<const std::string> args);

}
