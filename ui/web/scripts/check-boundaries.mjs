// Resolve imports with the same compiler settings as the independent projects.
import ts from 'typescript'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const config = ts.parseJsonConfigFileContent(ts.readConfigFile(path.join(root, 'tsconfig.json'), ts.sys.readFile).config, ts.sys, root)
const owner = file => {
  const relative = path.relative(root, file).replaceAll(path.sep, '/')
  return ['shared', 'src', 'server'].find(name => relative.startsWith(name + '/'))
}
const errors = []
for (const file of config.fileNames) {
  const sourceOwner = owner(file)
  if (!sourceOwner || file.endsWith('.json')) continue
  const source = ts.createSourceFile(file, fs.readFileSync(file, 'utf8'), ts.ScriptTarget.Latest, true)
  const visit = node => {
    let specifier
    if (ts.isImportDeclaration(node) || ts.isExportDeclaration(node)) specifier = node.moduleSpecifier
    if (ts.isCallExpression(node) && (node.expression.kind === ts.SyntaxKind.ImportKeyword || ts.isIdentifier(node.expression) && node.expression.text === 'require')) specifier = node.arguments[0]
    if (specifier && ts.isStringLiteralLike(specifier)) {
      const name = specifier.text
      const resolved = ts.resolveModuleName(name, file, config.options, ts.sys).resolvedModule?.resolvedFileName
      const dependency = resolved && owner(resolved)
      if (sourceOwner === 'shared' && (dependency === 'src' || dependency === 'server') || sourceOwner === 'src' && dependency === 'server' || sourceOwner === 'server' && dependency === 'src' || sourceOwner !== 'server' && name.startsWith('node:') || sourceOwner !== 'src' && /^react(?:$|[-/])/.test(name)) {
        errors.push(`${path.relative(root, file)}: ${sourceOwner} cannot import ${name}`)
      }
    }
    ts.forEachChild(node, visit)
  }
  visit(source)
}
if (errors.length) throw new Error(errors.join('\n'))
