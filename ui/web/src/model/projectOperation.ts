import { Builder, ByteBuffer } from 'flatbuffers'
import { api } from '../api'
import * as H from '../../shared/gen/tsugumi/host'
import * as R from '../../shared/gen/tsugumi/rpc'
import type { FailureT } from '../../shared/gen/tsugumi/data/failure'
import { RPCError, RPCFailure } from '../wire/rpc'

/** One host request. The bytes keep uint64 values and binary files lossless. */
export async function projectOperation(request: H.ProjectOperateRequestT): Promise<R.SaveResultT> {
  const builder = new Builder()
  builder.finish(request.pack(builder))
  const reply = await api<{ bytes: number[] }>('/api/project/operate', { bytes: Array.from(builder.asUint8Array()) })
  const result = H.ProjectOperationResult.getRootAsProjectOperationResult(new ByteBuffer(new Uint8Array(reply.bytes))).unpack()
  if (result.reply?.valueType === R.ReplyPayload.Failure) throw new RPCFailure(result.reply.value as FailureT)
  if (result.reply?.valueType !== R.ReplyPayload.SaveResult) throw new RPCError('protocol', 'Missing operation result')
  return result.reply.value as R.SaveResultT
}

export function operationInverse(result: R.SaveResultT): H.ProjectEditsT {
  if (!result.operationData.length) throw new RPCError('protocol', 'Missing operation changes')
  return H.ProjectEdits.getRootAsProjectEdits(new ByteBuffer(new Uint8Array(result.operationData))).unpack()
}
