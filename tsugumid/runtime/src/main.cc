#include "nix_eval.hh"
#include "tsugumi/core/daemon.hh"

int main(int argc, char** argv) {
  tsugumi::NixEvaluator::init_process();  // before any other thread (Boehm GC)
  return tsugumi::core::run_daemon(argc, argv);
}
