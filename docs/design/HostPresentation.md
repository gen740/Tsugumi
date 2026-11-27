# Host presentation services

`tsugumi-hostd` owns editor formatting and language-dependent version text for the Mac app, Web UI and Agent. The per-project `tsugumid` only records neutral version facts; see [Versions](../Versions.md).

## Editor formatting

`FileFormatRequest` takes a project-relative path and the current editor text. The extension selects the formatter and the path labels diagnostics. Both the editor and the Agent's `project_file_format` tool use this request to format unsaved text in memory. Empty diagnostic vectors may be omitted on the wire.

The host formats `.json` and `.nix`. JSON formatting is built in. For Nix, CMake sets `TSUGUMI_FORMAT_NIX` to the pinned nixfmt from nixpkgs or to its bundle-relative path in the Mac app. The Mac build relocates nixfmt and its libraries with `ui/macos/tools/bundle-formatters.py`.

nixfmt runs on a temporary copy with an explicit environment. Syntax errors return the original text and diagnostics labelled with the requested path.
