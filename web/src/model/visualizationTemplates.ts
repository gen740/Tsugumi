import files from '../../../ui/templates/visualization.json'
import { L } from '../i18n'

/** The Visualization starting points: one ordinary Python Card per plotting
 *  library. The drawing is written out in src/main.py so it can be read and
 *  changed there; `kind` (line, scatter or histogram) is a param. The files
 *  come from ui/templates/visualization.py, the same source as the Mac app's. */
export type VisualizationTemplate = keyof typeof files
/** The files of each starting point, by path inside the Card's definition folder. */
export const visualizationFiles: Record<VisualizationTemplate, Record<string, string>> = files
export const visualizationTemplates: { id: VisualizationTemplate; label: string }[] = [
  { id: 'visualization_matplotlib', label: L('Matplotlib（PNG / SVG / PDF）') },
  { id: 'visualization_plotly', label: L('Plotly（対話型 HTML）') },
  { id: 'visualization_altair', label: L('Altair（対話型 HTML）') },
]
