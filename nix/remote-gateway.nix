{
  core,
  makeWrapper,
  python3,
  runCommand,
}:
runCommand "tsugumi-remote-gateway" { nativeBuildInputs = [ makeWrapper ]; } ''
  mkdir -p "$out/bin" "$out/libexec"
  install -m644 ${../remote/remote_job.py} "$out/libexec/remote_job.py"
  install -m644 ${../remote/remote_gateway.py} "$out/libexec/remote_gateway.py"
  makeWrapper ${python3}/bin/python3 "$out/bin/tsugumi-remote-gateway" \
    --add-flags "$out/libexec/remote_gateway.py" \
    --set-default TSUGUMI_REMOTE_WORKER ${core}/bin/tsugumi-remote-worker
''
