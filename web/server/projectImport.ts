/** Import reads use the Host's project-copy traversal; HTTP transports bytes as base64. */
export async function importProject(folder: string, request: <T>(method: string, fields: unknown) => Promise<T>) {
  const files = new Map<string, Buffer[]>()
  let file_index = 0, offset = 0, name = ''
  for (;;) {
    const result = await request<{ name: string; files: { path: string; bytes: number[] }[]; nextFile: number; nextOffset: number; complete: boolean }>(
      'HostProjectReadRequest', { path: folder, file_index, offset })
    name = result.name
    for (const file of result.files) {
      const chunks = files.get(file.path) ?? []
      chunks.push(Buffer.from(file.bytes))
      files.set(file.path, chunks)
    }
    if (result.complete) break
    file_index = result.nextFile
    offset = result.nextOffset
  }
  return { name, files: Object.fromEntries([...files].map(([p, chunks]) => [p, Buffer.concat(chunks).toString('base64')])) }
}
