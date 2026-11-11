import files from '../../../ui/templates/visualization.json'
import { L } from '../i18n'

export type VisualizationTemplate = keyof typeof files
export const visualizationFiles: Record<VisualizationTemplate, Record<string, string>> = files
export const visualizationLibraries = [
  { id: 'matplotlib', label: L('Matplotlib（PNG / SVG / PDF）') },
  { id: 'plotly', label: L('Plotly（対話型 HTML）') },
  { id: 'altair', label: L('Altair（対話型 HTML）') },
]
const shapes = [
  { id: 'line', label: L('折れ線') }, { id: 'scatter', label: L('散布図') }, { id: 'histogram', label: L('ヒストグラム') },
]
export const visualizationTemplates = visualizationLibraries.flatMap(library => shapes.map(shape => ({
  id: `visualization_${library.id}_${shape.id}` as VisualizationTemplate,
  label: `${library.label} · ${shape.label}`, library: library.id, shape: shape.label,
})))
