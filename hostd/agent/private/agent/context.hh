// Model-only projections; durable messages remain the person's full record.
#pragma once

#include <cstddef>

#include "agent/provider.hh"

namespace tsugumi::agent {
inline constexpr size_t kToolResultBytes = 20 * 1024;
inline constexpr size_t kResultPageBytes = kToolResultBytes / 10;

// File pages and shell head/tail budgets share the general send-time cap.
auto FileResultForModel(std::string_view raw, size_t limit,
                        std::string_view reference) -> std::string;

// The marker is included in the byte budget. Boundaries preserve UTF-8.
auto ClipMiddle(std::string_view text, size_t limit, std::string_view reference)
    -> std::string;
auto RequestMessages(const std::vector<Message>& messages,
                     size_t omitted_before,
                     size_t result_limit = kToolResultBytes)
    -> std::vector<Message>;
auto Resources(const std::vector<Message>& messages)
    -> std::vector<std::string>;

// Includes replacements at the same count, but not ordinary append checkpoints.
auto RemovedMessages(const std::vector<Message>& before,
                     const std::vector<Message>& after) -> std::vector<Message>;
}  // namespace tsugumi::agent
