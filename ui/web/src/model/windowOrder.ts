// One stacking order for every in-page window: detail windows and Card view
// windows. It starts above the canvas overlays (z-index 20–50), below popovers and sheets.
let top = 1000

export const raiseWindow = () => ++top
export const topWindow = () => top
