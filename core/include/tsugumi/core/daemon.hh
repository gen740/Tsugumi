// Public entry point of the Tsugumi core daemon (tsugumid).
#pragma once

namespace tsugumi::core {

// Parses command line options, serves one project until SIGTERM/SIGINT and
// returns the process exit status.
int run_daemon(int argc, char** argv);

}  // namespace tsugumi::core
