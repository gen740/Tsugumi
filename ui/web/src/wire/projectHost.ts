import * as H from '../../shared/gen/tsugumi/host'
import * as R from '../../shared/gen/tsugumi/rpc'
import { language } from '../i18n'
import { hostCall } from './host'
import { s } from './rpc'

export const projectHost = {
  trust: (path: string, decision = H.ProjectTrustDecision.Unknown) =>
    hostCall<H.ProjectTrustT>(H.RequestPayload.ProjectTrustRequest,
      new H.ProjectTrustRequestT(path, decision), H.ReplyPayload.ProjectTrust),
  graph: (project: string) =>
    hostCall<R.VersionGraphT>(H.RequestPayload.HostVersionGraphRequest,
      new H.HostVersionGraphRequestT(project, language()), H.ReplyPayload.VersionGraph),
  async diff(project: string, commit: string, from?: string) {
    const result = await hostCall<H.VersionDiffT>(H.RequestPayload.HostVersionDiffRequest,
      new H.HostVersionDiffRequestT(project, commit, from ?? null), H.ReplyPayload.VersionDiff)
    return { text: s(result.text), truncated: result.truncated }
  },
  async comments(project: string, commit: string) {
    const result = await hostCall<H.VersionCommentsT>(H.RequestPayload.VersionCommentsRequest,
      new H.VersionCommentsRequestT(project, commit), H.ReplyPayload.VersionComments)
    return result.comments
  },
}
