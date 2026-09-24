import MarkdownIt from 'markdown-it';
import hljs from 'highlight.js/lib/core';
import javascript from 'highlight.js/lib/languages/javascript';
import typescript from 'highlight.js/lib/languages/typescript';
import cpp from 'highlight.js/lib/languages/cpp';
import python from 'highlight.js/lib/languages/python';
import json from 'highlight.js/lib/languages/json';
import { parseDocument } from 'htmlparser2';

hljs.registerLanguage('javascript', javascript); hljs.registerLanguage('typescript', typescript);
hljs.registerLanguage('cpp', cpp); hljs.registerLanguage('python', python); hljs.registerLanguage('json', json);
export const markdown = new MarkdownIt({ html: false, linkify: true, breaks: true, typographer: false });
export interface MdNode { tag: string; attrs: Record<string, any>; children: Array<MdNode | string>; code?: string; language?: string }
const node = (tag: string, attrs: Record<string, any> = {}, children: Array<MdNode | string> = []): MdNode => ({ tag, attrs, children });
export function safeLink(url: string): string | undefined {
  return /^https?:\/\//i.test(url) && !/[\u0000-\u0020]/.test(url) ? url : undefined;
}
function highlight(code: string, language: string): Array<MdNode | string> {
  if (!hljs.getLanguage(language)) return [code];
  const html = hljs.highlight(code, { language, ignoreIllegals: true }).value;
  const walk = (items: any[]): Array<MdNode | string> => items.flatMap(item => {
    if (item.type === 'text') return [item.data];
    if (item.type === 'tag' && item.name === 'span') return [node('span', { class: item.attribs.class || '' }, walk(item.children))];
    return [];
  });
  return walk(parseDocument(html).children);
}
export function parseMarkdown(source: string): MdNode[] {
  const tokens = markdown.parse(source.slice(0, 65536), {});
  const root = node('div'), stack = [root];
  const add = (value: MdNode | string) => stack[stack.length - 1].children.push(value);
  const visit = (items: typeof tokens) => {
    for (const token of items) {
      if (token.type === 'inline') { visit(token.children || []); continue; }
      if (token.type === 'text' || token.type === 'html_inline' || token.type === 'html_block') { add(token.content); continue; }
      if (token.type === 'softbreak' || token.type === 'hardbreak') { add(node('br')); continue; }
      if (token.type === 'code_inline') { add(node('code', { class: 'md-inline-code' }, [token.content])); continue; }
      if (token.type === 'fence' || token.type === 'code_block') {
        const language = token.info.trim().split(/\s+/)[0].toLowerCase();
        add({ ...node('div', { class: 'md-code-block' }, [node('pre', {}, highlight(token.content, language))]), code: token.content, language: language || 'text' });
        continue;
      }
      if (token.type === 'image') {
        const src = token.attrGet('src') || '', alt = token.content || 'Image';
        if (src === 'hello_world.png') add(node('img', { src, alt, class: 'md-image' }));
        else add(node('span', { class: 'md-image-label' }, [`[Image: ${alt}]`]));
        continue;
      }
      if (token.nesting === -1) { if (stack.length > 1) stack.pop(); continue; }
      if (token.nesting === 1) {
        const tag = token.tag;
        let element: MdNode;
        if (tag === 'a') {
          const href = safeLink(String(token.attrGet('href') || ''));
          element = node(href ? 'a' : 'span', href ? { href, class: 'md-link' } : {});
        } else if (['table', 'thead', 'tbody', 'tr', 'th', 'td'].includes(tag)) {
          element = node('div', { class: `md-${tag}` });
          const alignment = token.attrGet('style');
          if (alignment) element.attrs.style = { textAlign: String(alignment).split(':')[1] };
        } else if (tag === 'ul' || tag === 'ol') {
          element = node('div', { class: `md-${tag}`, start: Number(token.attrGet('start') || 1) });
        } else if (tag === 'li') {
          const list = stack[stack.length - 1];
          const marker = list.attrs.class === 'md-ol' ? `${list.attrs.start + list.children.length}.` : '\u2022';
          const body = node('div', { class: 'md-li-body' });
          add(node('div', { class: 'md-li' }, [node('span', { class: 'md-marker' }, [marker]), body]));
          stack.push(body); continue;
        } else element = node(['p', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'blockquote', 'strong', 'em', 's'].includes(tag) ? tag : 'span');
        add(element); stack.push(element); continue;
      }
      if (token.type === 'hr') add(node('div', { class: 'md-rule' }));
    }
  };
  visit(tokens);
  const fixTables = (element: MdNode) => {
    if (element.attrs.class === 'md-tr') {
      element.attrs.style = { gridTemplateColumns: `repeat(${element.children.length}, minmax(160px, 1fr))`, minWidth: `${element.children.length * 160}px` };
    }
    for (const child of element.children) if (typeof child !== 'string') fixTables(child);
  };
  fixTables(root);
  return root.children as MdNode[];
}
