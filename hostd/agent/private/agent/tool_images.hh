#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "agent/provider.hh"

namespace tsugumi::agent {

struct ImageLimits {
  size_t encoded_bytes = 10000000;
  size_t total_encoded_bytes = 20000000;
  size_t images = 16;
  uint32_t dimension = 8192;
  uint64_t pixels = 32000000;
};

struct ImageResult {
  std::string text;
  std::vector<Image> images;
};

// Extract complete, bounded PNG/JPEG values from public tool-result JSON.
// Source bytes remain unchanged in project RPC; the model-facing text gets an
// explanation instead of a megabyte-scale base64 or numeric byte vector.
auto SplitToolImages(std::string_view result, ImageLimits limits = {})
    -> ImageResult;

}  // namespace tsugumi::agent
