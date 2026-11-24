#pragma once

#include <string>
#include <string_view>

namespace tsugumi {

auto evaluate_literal_params(const std::string& def_dir, std::string_view text)
    -> std::string;
auto rewrite_param_literal(const std::string& path, std::string_view text,
                           std::string_view name, std::string_view literal)
    -> std::string;
}  // namespace tsugumi
