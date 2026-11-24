import SwiftUI
import TsugumiAgentClient

struct WorkerBitsEditor: View {
  @Binding var bits: [UInt8]

  var body: some View {
    HStack {
      if !bits.isEmpty {
        Text(bits.map(String.init).joined(separator: ", ")).monospaced()
      }
      Menu("ビットを切り替える…") {
        ForEach(WorkerBits.range, id: \.self) { bit in
          Toggle(String(bit), isOn: Binding(get: { bits.contains(bit) }, set: { _ in
            bits = WorkerBits.toggling(bit, in: bits)
          }))
        }
      }
      .fixedSize()
    }
  }
}
