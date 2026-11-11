# Installs the `tsugumi` command on Windows, without administrator rights:
#
#   irm https://github.com/gen740/Tsugumi/releases/latest/download/install.ps1 | iex
#
# Tsugumi runs in WSL2 (tsugumid needs Linux: Nix and bubblewrap): this runs
# install.sh in the default WSL distribution, which needs Nix, and puts
# %USERPROFILE%\.local\bin\tsugumi.cmd here, which starts it there. WSL forwards
# 127.0.0.1 to Windows, so the page opens in the browser of Windows.
#
# Checked on Windows with WSL2 up to opening a demo (#159).
#
# wsl.exe --exec runs no shell of its own, so nothing is expanded twice. ASCII
# only, with no double quote inside an argument: `irm | iex` does not read this
# file as UTF-8, and Windows PowerShell passes such quotes on wrongly.
#
#   $env:TSUGUMI_VERSION       a version to install (e.g. 0.6.0) instead of the latest
#   $env:TSUGUMI_INSTALL_BASE  where the files are, as WSL sees it, instead of the GitHub release
$ErrorActionPreference = 'Stop'

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
  throw 'tsugumi: WSL2 is needed. Run "wsl --install" to install Linux, then run this again.'
}
# A login shell: Nix's installer puts it on PATH through the profile.
wsl.exe --exec bash -lc 'command -v nix >/dev/null'
if ($LASTEXITCODE -ne 0) {
  throw 'tsugumi: Nix is needed in WSL (or no Linux is installed). Install it inside WSL as https://nixos.org/download says, then run this again.'
}

if ($env:TSUGUMI_INSTALL_BASE) {
  $base = $env:TSUGUMI_INSTALL_BASE
} elseif ($env:TSUGUMI_VERSION) {
  $base = "https://github.com/gen740/Tsugumi/releases/download/v$($env:TSUGUMI_VERSION)"
} else {
  $base = 'https://github.com/gen740/Tsugumi/releases/latest/download'
}

# install.sh checks the checksum and leaves everything in WSL's ~/.local.
wsl.exe --exec env "TSUGUMI_INSTALL_BASE=$base" bash -lc 'curl -fsSL $TSUGUMI_INSTALL_BASE/install.sh | sh'
if ($LASTEXITCODE -ne 0) { throw 'tsugumi: the installation in WSL failed (see the output above).' }

$bin = Join-Path $env:USERPROFILE '.local\bin'
New-Item -ItemType Directory -Force -Path $bin | Out-Null
$command = Join-Path $bin 'tsugumi.cmd'
# From WSL's home folder; a project folder is given as WSL sees it, as an absolute path.
Set-Content -Path $command -Encoding Ascii -Value @'
@echo off
wsl.exe --cd ~ --exec .local/bin/tsugumi %*
if errorlevel 1 exit /b %errorlevel%
if "%~1"=="uninstall" if "%~2"=="--yes" if "%~3"=="" (del "%~f0" & exit /b)
'@

Write-Host "tsugumi: installed $command"
if (($env:PATH -split ';') | Where-Object { $_.TrimEnd('\') -ieq $bin }) {
  Write-Host 'Run tsugumi to open the Web UI in the browser.'
} else {
  Write-Host "tsugumi: $bin is not on PATH (nothing was changed). Run $command, or add $bin to PATH."
}
