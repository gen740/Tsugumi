#include <signal.h>

#include <cstdio>
#include <string_view>

#include "tsugumi/workerd/local.hh"

#ifdef __linux__
auto remote_workerd_main(int argc, char** argv) -> int;
#endif

auto main(int argc, char** argv) -> int {
  if (argc >= 2 && std::string_view(argv[1]) == "--local") {
    using namespace tsugumi::workerd;
    if (argc != 3 + static_cast<int>(kLocalWorkerChannels)) return 2;
    ::signal(SIGPIPE, SIG_IGN);
    try {
      std::array<int, kLocalWorkerChannels> fds;
      for (size_t i = 0; i < fds.size(); ++i) fds[i] = std::stoi(argv[3 + i]);
      serve_local(fds, argv[2]);
      return 0;
    } catch (const std::exception& error) {
      std::fprintf(stderr, "tsugumi-workerd: %s\n", error.what());
      return 1;
    }
  }
#ifdef __linux__
  return remote_workerd_main(argc, argv);
#else
  std::fprintf(stderr, "tsugumi-workerd: remote reception requires Linux\n");
  return 2;
#endif
}
