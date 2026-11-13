# Origin of the guest kernel's configuration

`base-6.12.28.config` is, byte for byte, the configuration embedded
(`CONFIG_IKCONFIG`) in `vmlinux-6.12.28-153`, the kernel of Kata Containers 3.17.0
(`opt/kata/share/kata-containers/` of
[kata-static-3.17.0-arm64.tar.xz](https://github.com/kata-containers/kata-containers/releases/download/3.17.0/kata-static-3.17.0-arm64.tar.xz)).
That is the default kernel of Apple `container` 0.6.0, the version
`versions.env` pins
([DefaultsStore.swift](https://github.com/apple/container/blob/0.6.0/Sources/ContainerPersistence/DefaultsStore.swift)).

The file is the output of Linux's Kconfig for Kata's configuration; Kata Containers is under
Apache-2.0. `kernel.nix` applies its changes to it when it builds, and the result
is `kernel-config` in `licenses.tar.gz` of the boot disk's release.
