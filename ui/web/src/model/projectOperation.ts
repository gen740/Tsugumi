import { ByteBuffer } from 'flatbuffers'
import { hostCall } from '../wire/host'
import * as H from '../../shared/gen/tsugumi/host'
import * as R from '../../shared/gen/tsugumi/rpc'
import type { FailureT } from '../../shared/gen/tsugumi/data/failure'
import { RPCError, RPCFailure } from '../wire/rpc'

/** One host request. The bytes keep uint64 values and binary files lossless. */
export async function projectOperation(request: H.ProjectOperateRequestT): Promise<R.SaveResultT> {
  const result = await hostCall<H.ProjectOperationResultT>(H.RequestPayload.ProjectOperateRequest, request, H.ReplyPayload.ProjectOperationResult)
  if (result.reply?.valueType === R.ReplyPayload.Failure) throw new RPCFailure(result.reply.value as FailureT)
  if (result.reply?.valueType !== R.ReplyPayload.SaveResult) throw new RPCError('protocol', 'Missing operation result')
  return result.reply.value as R.SaveResultT
}

export function operationInverse(result: R.SaveResultT): H.ProjectEditsT {
  if (!result.operationData.length) throw new RPCError('protocol', 'Missing operation changes')
  return H.ProjectEdits.getRootAsProjectEdits(new ByteBuffer(new Uint8Array(result.operationData))).unpack()
}
