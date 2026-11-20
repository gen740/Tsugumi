# Origin of the guest kernel's configuration

`base-6.12.28.config` is, byte for byte, the configuration embedded
(`CONFIG_IKCONFIG`) in `vmlinux-6.12.28-153`, the kernel of Kata Containers 3.17.0
(`opt/kata/share/kata-containers/` of
[kata-static-3.17.0-arm64.tar.xz](https://github.com/kata-containers/kata-containers/releases/download/3.17.0/kata-static-3.17.0-arm64.tar.xz)).
That is the default kernel of Apple `container` 0.6.0, the version
`versions.env` pins
([DefaultsStore.swift](https://github.com/apple/container/blob/0.6.0/Sources/ContainerPersistence/DefaultsStore.swift)).

The file is the output of Linux's Kconfig for Kata's configuration.
`kernel.nix` applies its changes to it when it builds.

## Rosetta TSO

`rosetta-tso-6.12.patch` ports Apple's [Rosetta Enhancement Patch](https://developer.apple.com/download/files/RosettaPatch.zip)
from Linux 6.10 to 6.12.115. The original license is in
`rosetta-tso-LICENSE.txt`. The port places the thread field and switch hook
alongside the new arm64 permission overlay state, links `tso.o` when enabled,
and clears both the hardware bit and its saved thread state on exec. It
preserves Apple's per-thread `PR_SET_MEM_MODEL` ABI and CPU feature detection.
