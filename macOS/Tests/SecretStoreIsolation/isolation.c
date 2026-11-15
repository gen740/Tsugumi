#include <stdio.h>
#include <stdlib.h>

// Run before Swift globals or test discovery can start the app's model catalog.
__attribute__((constructor)) void tsugumi_test_secret_store_isolation(void) {
  setenv("TSUGUMI_TEST_NO_SECRET_STORE", "1", 1);
  if (getenv("TSUGUMI_TEST_RUN_ROOT") == NULL) {
    fputs("Run Swift tests through tsugumi-test-env or tests/run-isolated.py\n", stderr);
    exit(1);
  }
}
