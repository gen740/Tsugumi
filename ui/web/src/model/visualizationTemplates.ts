import { L } from '../i18n'

/** Presentation labels only; the host serves the Card files. */
export type VisualizationTemplate = 'visualization_matplotlib' | 'visualization_plotly' | 'visualization_altair'
export const visualizationTemplates: { id: VisualizationTemplate; label: string }[] = [
  { id: 'visualization_matplotlib', label: L('Matplotlib（PNG / SVG / PDF）') },
  { id: 'visualization_plotly', label: L('Plotly（対話型 HTML）') },
  { id: 'visualization_altair', label: L('Altair（対話型 HTML）') },
]
