import Foundation

/// Steps of starting the VM; the guest reports the later ones on the console.
public enum VMStage: String, Sendable, CaseIterable {
    case image, disk, boot, store, nix, network, checkingStore, checkingStoreUpgrade, supervisor, ready
  }

public enum VMState: Sendable, Equatable {
    case stopped
    case starting(VMStage)
    case running
    case failed(String)
  }

