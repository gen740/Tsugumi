{
  core,
  makeWrapper,
  nix,
  python3,
  runCommand,
}:
runCommand "tsugumi-remote-gateway" { nativeBuildInputs = [ makeWrapper ]; } ''
  mkdir -p "$out/bin" "$out/libexec"
  install -m644 ${../remote/remote_job.py} "$out/libexec/remote_job.py"
  install -m644 ${../remote/remote_gateway.py} "$out/libexec/remote_gateway.py"
  # Non-interactive SSH may not load the host's Nix profile. The worker's
  # embedded evaluator invokes nix for its build hook.
  makeWrapper ${python3}/bin/python3 "$out/bin/tsugumi-remote-gateway" \
    --add-flags "$out/libexec/remote_gateway.py" \
    --prefix PATH : ${nix}/bin \
    --set-default TSUGUMI_REMOTE_WORKER ${core}/bin/tsugumi-remote-worker
''
