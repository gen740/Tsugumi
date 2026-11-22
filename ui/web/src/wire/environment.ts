import * as H from '../../shared/gen/tsugumi/host'
import { type HistoryVersionMismatch, historyVersionMismatch } from '../../shared/historyVersion'
import { L } from '../i18n'
import { hostCall } from './host'
import { s } from './rpc'

// Words for hostd's stage names are presentation: each UI keeps its own text
// rather than hostd sending it.
const STAGES: Record<string, string> = {
  'installing the VM image': 'VM イメージを展開しています',
  'creating the Nix store disk': 'Nix store の保存領域を作っています',
  'booting the VM': 'VM を起動しています',
  'mounting the Nix store disk': 'Nix store の保存領域を開いています（初回は初期化）',
  'checking the Nix store after an unclean stop': '前回の VM が正常に停止しなかったため、Nix store を検査・修復しています',
  'checking the Nix store contents': 'Nix store の内容を検査・修復しています（初回と VM イメージの更新後）',
  'starting the Nix daemon': 'Nix を準備しています',
  'configuring the network': 'ネットワークを設定しています',
  'starting the supervisor': '実行環境の管理プロセスを起動しています',
  'starting tsugumid': 'このプロジェクトの tsugumid を起動しています',
  'waiting for tsugumid to become ready': 'tsugumid の応答を待っています',
}

export interface EnvPublicState {
  executionScopeUnavailable?: boolean
  state: 'stopped' | 'starting' | 'ready' | 'failed'
  message?: string
  /** While starting in the VM: the step, and how far along the start is. */
  stage?: string
  fraction?: number
  recovery?: string
  log?: string
  historyVersionMismatch?: HistoryVersionMismatch
  endpoint?: string
  token?: string
  projectId?: string
  readyFile?: string
}

export function environmentState(value: H.EnvironmentStateT): EnvPublicState {
  switch (value.phase) {
    case H.EnvironmentPhase.Ready:
      return { state: 'ready', endpoint: s(value.endpoint), token: s(value.token), projectId: s(value.projectId),
        readyFile: s(value.readyFile), executionScopeUnavailable: value.executionScopeUnavailable }
    case H.EnvironmentPhase.Starting:
      return { state: 'starting', message: L('実行環境を起動しています') }
    case H.EnvironmentPhase.Failed: {
      const message = s(value.message)
      const error = value.startupError
      const mismatch: HistoryVersionMismatch | undefined = error
        ? historyVersionMismatch({ kind: s(error.kind), found: s(error.found), expected: s(error.expected) })
        : undefined
      return { state: 'failed', message, recovery: s(value.recovery), log: s(value.log), historyVersionMismatch: mismatch }
    }
    default:
      return { state: 'stopped' }
  }
}

export function environmentProgress(value: H.EnvironmentProgressT): EnvPublicState {
  const stage = s(value.stage)
  return { state: 'starting', message: STAGES[stage] ? L(STAGES[stage]) : stage, stage, fraction: value.fraction ?? undefined }
}

async function request(project: string, action: H.EnvironmentAction) {
  return environmentState(await hostCall<H.EnvironmentStateT>(H.RequestPayload.EnvironmentRequest,
    new H.EnvironmentRequestT(project, action, action === H.EnvironmentAction.Start ? [location.origin] : []),
    H.ReplyPayload.EnvironmentState))
}

export const environmentHost = {
  start: (project: string) => request(project, H.EnvironmentAction.Start),
  stop: (project: string) => request(project, H.EnvironmentAction.Stop),
  async list() {
    const result = await hostCall<H.EnvironmentListT>(H.RequestPayload.EnvironmentListRequest,
      new H.EnvironmentListRequestT(), H.ReplyPayload.EnvironmentList)
    return result.environments.map((entry) => ({
      name: s(entry.name), key: s(entry.state!.key), state: environmentState(entry.state!),
    }))
  },
}
