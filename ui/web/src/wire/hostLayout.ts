import * as H from '../../shared/gen/tsugumi/host'
import { api } from '../api'
export { H, sceneRequest } from '../../shared/hostLayout'
export const layoutHost = {
  complete: async (project: string, layout: string, definitions: Record<string, string>) =>
    (await api<{ json: string }>('/api/layout/complete', { project_json: project, layout_json: layout, definitions_json: JSON.stringify(definitions) })).json,
  displayGeometry: () => api<H.DisplayGeometriesT>('/api/layout/display-geometry', {}),
  validate: async (json: string, removedCards: string[] = []) => (await api<{ json: string }>('/api/layout/validate', { json, removedCards })).json,
  versionCanvas: (data: unknown) => api<H.VersionCanvasT>('/api/layout/version-canvas', data),
}
