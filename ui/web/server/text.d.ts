// Text files bundled as strings (esbuild's text loader).
declare module '*.md' {
  const text: string
  export default text
}
