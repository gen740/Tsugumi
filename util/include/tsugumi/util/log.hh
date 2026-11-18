// Structured stderr logging. Never pass secrets (tokens) to these functions.
#pragma once

#include <string>

namespace tsugumi::log {

void info(const std::string& msg);
void warn(const std::string& msg);
void error(const std::string& msg);

}  // namespace tsugumi::log
