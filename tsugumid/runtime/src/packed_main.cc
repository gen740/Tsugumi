#include <cstdio>
#include <string_view>

#include "tsugumi/core/daemon.hh"

int main(int argc, char** argv) {
  for (int index = 1; index + 1 < argc; ++index)
    if (std::string_view(argv[index]) == "--packed-builds")
      return tsugumi::core::run_daemon(argc, argv);
  std::fputs("tsugumid-pack requires --packed-builds\n", stderr);
  return 2;
}
