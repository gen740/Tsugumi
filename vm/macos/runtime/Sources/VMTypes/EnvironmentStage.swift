import Foundation

public enum EnvironmentStage: String, Sendable, CaseIterable {
  case vmImage = "installing the VM image"
  case vmDisk = "creating the Nix store disk"
  case vmBoot = "booting the VM"
  case vmStore = "mounting the Nix store disk"
  case vmNix = "starting the Nix daemon"
  case vmNetwork = "configuring the network"
  case vmCheckingStore = "checking the Nix store after an unclean stop"
  case vmCheckingStoreUpgrade = "checking the Nix store contents"
  case vmSupervisor = "starting the supervisor"
  case startingDaemon = "starting tsugumid"
  case connecting = "waiting for tsugumid to become ready"

  public init(vmStage vm: VMStage) {
    switch vm {
    case .image: self = .vmImage
    case .disk: self = .vmDisk
    case .boot: self = .vmBoot
    case .store: self = .vmStore
    case .nix: self = .vmNix
    case .network: self = .vmNetwork
    case .checkingStore: self = .vmCheckingStore
    case .checkingStoreUpgrade: self = .vmCheckingStoreUpgrade
    case .supervisor, .ready: self = .vmSupervisor
    }
  }
}

