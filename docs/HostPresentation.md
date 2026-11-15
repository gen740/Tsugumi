# Host presentation services

`tsugumi-hostd` owns editor formatting and language-dependent version text for the Mac app, Web UI and Agent. The per-project `tsugumid` only records neutral version facts; see [Versions](Versions.md).

## Editor formatting

`host.fbs` defines `FileFormatRequest` (path, editor text, optional language) and `FileFormatResult` (text and diagnostics). A path selects a language and labels diagnostics; the formatter never reads or saves that project file. The Agent's `project_file_format` tool calls this same implementation. Empty diagnostic vectors may be omitted on the wire.

JSON formatting is built in. C++, Python, Nix and Bash use clang-format, Ruff, nixfmt and shfmt respectively. CMake fixes their absolute executable paths with `TSUGUMI_FORMAT_CLANG`, `TSUGUMI_FORMAT_RUFF`, `TSUGUMI_FORMAT_NIX` and `TSUGUMI_FORMAT_SH`. `nix/core.nix` supplies tools from the locked nixpkgs. The native Mac build uses these same pinned native tools, bundles their dylibs with `macOS/tools/bundle-formatters.py`, and resolves them beside the app's resources. An unconfigured tool is unavailable; there is no executable search or fallback to another formatter.

Formatting uses explicit style options, an isolated temporary directory and an explicit environment. Ruff ignores configuration files; clang-format uses its inline Google style (110 columns); shfmt ignores EditorConfig and uses Bash with two-space indentation. User and project configuration, locale and PATH do not choose formatting behavior. Tools receive a 20-second deadline. Failed syntax checks return the original text and diagnostics whose temporary filenames are replaced by the requested path. Tool versions and options are part of the host build, so identical input on that build has identical formatting regardless of the core's environment.
