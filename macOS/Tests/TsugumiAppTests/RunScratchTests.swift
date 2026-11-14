import FlatBuffers
import Foundation
import TsugumiRPC
import Testing

struct RunScratchTests {
  @Test func explicitSelectionSurvivesWireRoundTrip() throws {
    let selection = Tsugumi_RPC_RunScratchDeleteRequestT()
    selection.opId = UUID().uuidString.lowercased()
    selection.paths = ["runs/0f9e9e19-6abb-49bb-9e46-a9f8907ca917", "recovery"]
    let request = Tsugumi_RPC_RequestT()
    request.value = Tsugumi_RPC_RequestPayloadUnion(selection, type: .runscratchdeleterequest)
    let message = Tsugumi_RPC_MessageT()
    message.requestId = 17
    message.payload = Tsugumi_RPC_PayloadUnion(request, type: .request)
    var builder = FlatBufferBuilder(initialSize: 512)
    var object: Tsugumi_RPC_MessageT? = message
    let offset = Tsugumi_RPC_Message.pack(&builder, obj: &object)
    Tsugumi_RPC_Message.finish(&builder, end: offset)
    var buffer = ByteBuffer(data: builder.data)
    let root: Tsugumi_RPC_Message = try getCheckedRoot(byteBuffer: &buffer, fileId: "TSG1")
    let decoded = Tsugumi_RPC_MessageT(root)
    let restored = (decoded.payload?.value as? Tsugumi_RPC_RequestT)?.value?.value as? Tsugumi_RPC_RunScratchDeleteRequestT
    #expect(restored?.opId == selection.opId)
    #expect(restored?.paths == selection.paths)
  }
}
