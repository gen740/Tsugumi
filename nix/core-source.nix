{ lib }:
lib.fileset.toSource {
  root = ../.;
  fileset = lib.fileset.unions [
    ../VERSION
    ../CMakeLists.txt
    ../cmake
    ../workerd
    ../util
    ../rpc
    ../tsugumid
    ../hostd/CMakeLists.txt
    ../hostd/cmake
    ../hostd/src
    ../hostd/private
    ../hostd/agent/src
    ../hostd/agent/private
    ../vm/guest/CMakeLists.txt
    ../vm/guest/src
    ../client/CMakeLists.txt
    ../client/cmake
    ../client/include
    ../client/src
    ../sdk
    (lib.fileset.fileFilter (file: file.hasExt "json") ../ui/shared/templates)
    # Documentation tooling does not affect the native runtime.
    ../protocol/CMakeLists.txt
    (lib.fileset.fileFilter (file: file.hasExt "fbs") ../protocol)
    ../hostd/agent/Shared/system-prompt.md
    ../hostd/agent/Shared/guide-prompt.md
    ../hostd/agent/Shared/demo-skill.md
    ../hostd/agent/Shared/helpers-skill.md
    ../hostd/agent/Shared/review-skill.md
    ../hostd/agent/Shared/rerun-compare-skill.md
    ../hostd/agent/Shared/agent.json
    ../hostd/agent/Shared/guide-script.json
    ../hostd/agent/Shared/guide-script.en.json
  ];
}
