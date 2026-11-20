#include "nix_eval.hh"
#include "tsugumi/core/daemon.hh"

int main(int argc, char** argv) {
  tsugumi::NixEvaluator::run_subprocess_if_requested(argc, argv);
  if (auto result = tsugumi::NixEvaluator::build_client(argc, argv))
    return *result;
  tsugumi::NixEvaluator::init_process();  // before any other thread (Boehm GC)
  return tsugumi::core::run_daemon(argc, argv);
}
