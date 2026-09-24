import { computed, defineComponent, h, ref } from '@rmlui/vue';
import { parseMarkdown, type MdNode } from './markdown';
import { callHostJson } from '../../../../Frontend/src/runtime';

export default defineComponent({
  name: 'RmlMarkdown',
  props: { content: { type: String, required: true }, messageId: { type: Number, required: true } },
  setup(props) {
    const tree = computed(() => parseMarkdown(props.content));
    const copied = ref('');
    const render = (item: MdNode | string, key: string): any => {
      if (typeof item === 'string') return item;
      const attributes: Record<string, any> = { ...item.attrs, key, id: `md-${props.messageId}-${key}` };
      if (item.tag === 'a') attributes.onClick = () => { void callHostJson('chat.openLink', { url: attributes.href }); };
      const children = item.children.map((child, index) => render(child, `${key}-${index}`));
      if (item.code !== undefined) children.unshift(h('div', { class: 'md-code-header' }, [
        h('span', {}, item.language),
        h('button', { class: 'icon-button code-copy', id: `chat-code-copy-${props.messageId}-${key}`, title: copied.value === key ? 'Copied' : 'Copy code',
          onClick: async () => { await callHostJson('chat.copy', { text: item.code }); copied.value = key; setTimeout(() => { copied.value = ''; }, 1600); } },
        [h('img', { src: copied.value === key ? 'icons/check.png' : 'icons/copy.png' })]),
      ]));
      return h(item.tag, attributes, children);
    };
    return () => h('div', { class: 'markdown', id: `chat-markdown-${props.messageId}` }, tree.value.map((item, index) => render(item, String(index))));
  },
});
