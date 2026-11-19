#include "nix_eval.hh"
#include "tsugumi/core/daemon.hh"

int main(int argc, char** argv) {
  tsugumi::NixEvaluator::run_subprocess_if_requested(argc, argv);
  tsugumi::NixEvaluator::init_process();  // before any other thread (Boehm GC)
  return tsugumi::core::run_daemon(argc, argv);
}
