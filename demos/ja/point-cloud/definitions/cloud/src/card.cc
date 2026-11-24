#include "card.hh"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <random>
#include <stdexcept>
#include <vector>

namespace {

struct Point {
  float x, y, z;
  uint32_t rgba;
};

struct ChunkHeader {
  uint32_t flags;
  uint32_t count;
};

static_assert(sizeof(Point) == 16 && sizeof(ChunkHeader) == 8);
static_assert(std::endian::native == std::endian::little &&
              std::numeric_limits<float>::is_iec559);
constexpr int64_t kMaxPoints = 2'000'000;
constexpr size_t kChunkBytes = 1u << 20;
constexpr size_t kPointsPerChunk =
    (kChunkBytes - sizeof(ChunkHeader)) / sizeof(Point);

auto make_cloud(int64_t count, uint32_t seed) -> std::vector<Point> {
  std::mt19937 rng(seed);
  const auto uniform = [&] { return static_cast<double>(rng()) / 0x1p32; };
  constexpr double kTurn = 2.0 * std::numbers::pi;
  std::vector<Point> points;
  points.reserve(static_cast<size_t>(count));
  for (int64_t i = 0; i < count; ++i) {
    // Spread samples through the tube of a (2, 3) torus knot.
    const double t = uniform() * kTurn;
    const double angle = uniform() * kTurn;
    const double offset = 0.19 * std::sqrt(uniform());
    const double minor = 0.80 * std::cos(3.0 * t) + offset * std::cos(angle);
    const double z = 0.80 * std::sin(3.0 * t) + offset * std::sin(angle);
    const double major = 2.0 + minor;
    const double color = 0.5 + 0.5 * std::sin(3.0 * t);
    const double light = 1.0 - 0.4 * (offset / 0.19);
    const auto red = static_cast<uint32_t>((70.0 + 185.0 * color) * light);
    const auto green = static_cast<uint32_t>((185.0 - 65.0 * color) * light);
    const auto blue = static_cast<uint32_t>((255.0 - 105.0 * color) * light);
    points.push_back({static_cast<float>(major * std::cos(2.0 * t)),
                      static_cast<float>(major * std::sin(2.0 * t)),
                      static_cast<float>(z),
                      red | (green << 8) | (blue << 16) | 0xff000000u});
  }
  return points;
}

auto send_cloud(const tsugumi_view_session& session,
                const std::vector<Point>& points) -> void {
  std::vector<uint8_t> bytes(kChunkBytes);
  for (size_t offset = 0; offset < points.size(); offset += kPointsPerChunk) {
    const auto count = std::min(kPointsPerChunk, points.size() - offset);
    const ChunkHeader header{offset == 0 ? 1u : 0u,
                             static_cast<uint32_t>(count)};
    const auto payload = count * sizeof(Point);
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), points.data() + offset, payload);
    if (card::display::send_raw(
            session, {bytes.data(), sizeof(header) + payload}) != 0) {
      throw std::runtime_error("Could not send the point cloud");
    }
  }
}

}  // namespace

auto run(const Card::Inputs&) -> Card::Output {
  const auto count = Card::params.count;
  if (count < 1 || count > kMaxPoints) {
    throw std::invalid_argument("count must be between 1 and 2000000");
  }
  // These parameters belong to the build. A changed build starts a new worker.
  static const auto points =
      make_cloud(count, static_cast<uint32_t>(Card::params.seed));
  const auto opened =
      card::display::view_open("points", TSUGUMI_VIEW_WINDOW, 960, 720);
  send_cloud(opened.session, points);
  return {};
}
