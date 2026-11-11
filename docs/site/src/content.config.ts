import { defineCollection } from 'astro:content'
import { z } from 'astro/zod'
import path from 'node:path'
import { catalog, ROOT, STORY, titleOf, read, storyMarkdown, rewriteLinks, linker, markdownOut } from './lib/documents.mjs'

const documents = defineCollection({
  schema: z.object({
    source: z.string(),
    out: z.string(),
    title: z.string(),
    markdown: z.string(),
    story: z.boolean(),
    whole: z.string().optional(),
    previous: z.string().optional(),
    next: z.string().optional(),
  }),
  loader: {
    name: 'repository-documents',
    async load({ store, parseData, renderMarkdown, watcher, logger }) {
      async function sync() {
        const { entries, pages } = catalog()
        const loaded = []
        for (const entry of entries) {
          const { source, out, story } = entry
          const id = out.slice('pages/'.length, -'.html'.length)
          const position = story ? STORY.indexOf(story) : -1
          const data = await parseData({ id, data: {
            source, out, title: story?.title ?? titleOf(source),
            markdown: markdownOut(source, pages), story: Boolean(story),
            whole: story?.sections ? pages.get(source) : undefined,
            previous: position > 0 ? STORY[position - 1].out : undefined,
            next: position >= 0 ? STORY[position + 1]?.out : undefined,
          } })
          const body = story ? storyMarkdown(story) : read(source)
          const linked = rewriteLinks(body, linker(source, out, pages))
          const rendered = await renderMarkdown(linked)
          // Preserve the scrollable tables and full-width diagram characters used
          // by the shared site/app stylesheet without adding client JavaScript.
          rendered.html = rendered.html.replace(/<table(\s[^>]*)?>/g, '<div class="table-wrap"><table$1>').replace(/<\/table>/g, '</table></div>')
          rendered.html = rendered.html.replace(/(<pre[^>]*><code[^>]*>)([\s\S]*?)(<\/code><\/pre>)/g, (_, start, code, end) => {
            if (!/[─│┌┐└┘├┤┬┴┼]/.test(code)) return start + code + end
            return start + code.replace(/[\p{Script=Han}\p{Script=Hiragana}\p{Script=Katakana}\u3000-\u303f\u30fb\u30fc\uff01-\uff60]/gu, '<span class="wide">$&</span>') + end
          })
          loaded.push({ id, data, body, rendered })
        }
        store.clear()
        for (const entry of loaded) store.set(entry)
      }
      await sync()
      if (watcher) {
        const sources = [path.join(ROOT, 'README.md'), path.join(ROOT, 'THIRD_PARTY_NOTICES.md'), path.join(ROOT, 'docs')]
        watcher.add(sources)
        let pending = Promise.resolve()
        watcher.on('all', (event, file) => {
          if (!['add', 'change', 'unlink'].includes(event)) return
          if (!sources.includes(file) && !(path.dirname(file) === path.join(ROOT, 'docs') && file.endsWith('.md'))) return
          pending = pending.then(sync).catch(error => logger.error(error.message))
        })
      }
    },
  },
})

export const collections = { documents }
