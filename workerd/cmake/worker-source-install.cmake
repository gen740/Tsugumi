tsugumi_card_nix_files(worker "${PROJECT_SOURCE_DIR}/nix/worker.nix")
tsugumi_card_nix_files(worker/src src/native_main.cc src/display_channel.hh)
tsugumi_card_nix_files(worker/protocol
  "${TSUGUMI_PROTOCOL_DIR}/artifact.fbs"
  "${TSUGUMI_PROTOCOL_DIR}/card.fbs"
  "${TSUGUMI_PROTOCOL_DIR}/card_enums.fbs"
  "${TSUGUMI_PROTOCOL_DIR}/tsugumi_enums.fbs"
  "${TSUGUMI_PROTOCOL_DIR}/worker.fbs"
  "${TSUGUMI_PROTOCOL_DIR}/worker_types.fbs")
tsugumi_card_nix_files(worker/include/tsugumi "${TSUGUMI_PROTOCOL_DIR}/include/tsugumi/card_abi.h")
tsugumi_card_nix_files(worker/include/tsugumi/rpc "${PROJECT_SOURCE_DIR}/rpc/include/tsugumi/rpc/frame.hh")
tsugumi_card_nix_files(worker/include/tsugumi/workerd
  include/tsugumi/workerd/artifact_id.hh
  include/tsugumi/workerd/frame.hh
  include/tsugumi/workerd/view.hh)
