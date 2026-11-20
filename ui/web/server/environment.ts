// Host launch options and the environment calls needed by Agent/CLI routes.
import fs from 'node:fs'
import path from 'node:path'
import { EnvironmentPhase } from '../shared/gen/tsugumi/host/environment-phase'

export interface VMHostOptions { host: string; image: string; support: string }
export type HostEvent = { type: string; value: any }
type Request = <T = any>(method: string, fields?: unknown) => Promise<T>
export interface Binaries {
  tsugumid: string
  worker: string
  sdkDir: string
}

export interface Environments {
  readonly hostArguments?: string[]
  start(dir: string): Promise<{ phase: EnvironmentPhase; project_id?: string; message?: string }>
  attachFolders(dir: string, folders: string[]): Promise<{ folder: string; resource: string }[]>
  removeFolders(dir: string, folders: string[]): Promise<void>
}

export const projectKey = (dir: string) => fs.realpathSync(dir)

export function hostEnvironments(bins: Binaries, support: string, origins: () => string[], request: Request, vm?: VMHostOptions): Environments {
  return {
    hostArguments: vm
      ? ['--vmhost', vm.host, '--vm-support', vm.support, '--vm-image', vm.image]
      : ['--tsugumid', bins.tsugumid, '--worker', bins.worker, '--sdk-dir', bins.sdkDir,
         '--environment-dir', path.join(support, 'env')],
    start: (project) => request('EnvironmentRequest', { project, action: 'Start', origins: origins() }),
    attachFolders: async (project, folders) => (await request('AttachFoldersRequest', { project, folders })).folders,
    removeFolders: async (project, folders) => { await request('EnvironmentRequest', { project, action: 'RemoveFolders', folders }) },
  }
}
