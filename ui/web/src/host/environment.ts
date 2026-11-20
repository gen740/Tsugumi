// The environment calls the Agent routes need, over Host RPC.
import { EnvironmentPhase } from '../../shared/gen/tsugumi/host/environment-phase'

export type HostEvent = { type: string; value: any }
type Request = <T = any>(method: string, fields?: unknown) => Promise<T>

export interface Environments {
  state(dir: string): Promise<{ phase: EnvironmentPhase; project_id?: string; message?: string }>
  start(dir: string): Promise<{ phase: EnvironmentPhase; project_id?: string; message?: string }>
  attachFolders(dir: string, folders: string[]): Promise<{ folder: string; resource: string }[]>
  removeFolders(dir: string, folders: string[]): Promise<void>
}

/** hostd names projects by their canonical folder, which is what it gave the page. */
export const projectKey = (dir: string) => dir

export function hostEnvironments(request: Request, origins: () => string[]): Environments {
  return {
    state: (project) => request('EnvironmentRequest', { project, action: 'State' }),
    start: (project) => request('EnvironmentRequest', { project, action: 'Start', origins: origins() }),
    attachFolders: async (project, folders) => (await request('AttachFoldersRequest', { project, folders })).folders,
    removeFolders: async (project, folders) => { await request('EnvironmentRequest', { project, action: 'RemoveFolders', folders }) },
  }
}
