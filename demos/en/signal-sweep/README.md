# Signal detection sweep

Run `start` to compare signals 8, 12 and 16 with detection thresholds 10 and 14.

- `definitions/samples/default.nix` returns an ordered `run` list and shares the thresholds through `exports.thresholds`.
- `definitions/score/default.nix` reads `inputs.signal.thresholds` because the receiving port is named `signal`. Each Bash implementation reads one runtime signal from `$JSON_SIGNAL`.
- The six invocations enumerate all signal–threshold pairs. For each signal, threshold 10 runs at the first position and threshold 14 at the second, regardless of completion order.
- `collect` takes the batch on `in` and sends one typed message with an `items` vector on `out`. `definitions/summary/default.nix` reads the vector from `$JSON_RESULTS` and the shared threshold count from `inputs.results.thresholdCount`.

The summary reports three detections. Open the history for `score` to inspect the six inputs and outputs.
