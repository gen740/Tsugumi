{
  core,
  lib,
  makeWrapper,
  nix,
  python3,
  runCommand,
  util-linux,
}:
runCommand "tsugumi-remote-daemon" { nativeBuildInputs = [ makeWrapper ]; } ''
  mkdir -p "$out/bin" "$out/libexec"
  install -m644 ${../remote/remote_job.py} "$out/libexec/remote_job.py"
  install -m644 ${../remote/remote_gateway.py} "$out/libexec/remote_gateway.py"
  # The worker's embedded evaluator invokes nix for its build hook.
  makeWrapper ${python3}/bin/python3 "$out/bin/tsugumi-remote-daemon" \
    --add-flags "$out/libexec/remote_gateway.py" \
    --prefix PATH : ${lib.makeBinPath [ nix util-linux ]} \
    --set-default TSUGUMI_REMOTE_WORKER ${core}/bin/tsugumi-remote-worker
''
