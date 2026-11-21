export function onData(root, initial, render) {
  const draw = ({ config, blobs }) => render(JSON.parse(config), blobs)
  draw(initial)
  root.addEventListener('tsugumi:data', event => draw(event.detail))
}
