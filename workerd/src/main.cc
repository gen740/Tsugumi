#include <signal.h>

#include <cstdio>
#include <string_view>

#include "tsugumi/workerd/local.hh"

#ifdef __linux__
auto remote_workerd_main(int argc, char** argv) -> int;
#endif

auto main(int argc, char** argv) -> int {
  if (argc == 4 && std::string_view(argv[1]) == "--local") {
    ::signal(SIGPIPE, SIG_IGN);
    try {
      tsugumi::workerd::serve_local(std::stoi(argv[2]), argv[3]);
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
