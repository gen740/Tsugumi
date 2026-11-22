---
description: Pin the real MNIST with Nix
---

Fetches the four MNIST files in IDX format (60000 training and 10000 test images) from the public mirror https://ossci-datasets.s3.amazonaws.com/mnist/ with fetchurl in default.nix, pinning their content by hash. The download happens only the first time; no network while running. Checks the headers and counts, shows samples of each digit, and passes the store folder's path on.
