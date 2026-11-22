import { onData } from './dom.js'

export default function mount(root, data) {
  const image = document.createElement('img')
  image.draggable = false
  image.style.cssText = 'width:100%;height:100%;object-fit:contain;display:block'
  root.append(image)
  let url
  onData(root, data, (config, blobs) => {
    if (url) URL.revokeObjectURL(url)
    url = URL.createObjectURL(new Blob([blobs.image], { type: config.mediaType }))
    image.alt = config.alt
    image.src = url
  })
}
