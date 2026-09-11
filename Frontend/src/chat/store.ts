import { reactive, watchEffect } from '@rmlui/vue';
import { native } from '../bridge';
import { callHostJson } from '../runtime';

export interface Message { id: number; role: 'user' | 'assistant'; content: string; status?: string; error?: string }
export interface Conversation { id: number; title: string; messages: Message[] }
const defaults = { conversations: [] as Conversation[], activeId: 0, nextId: 1, draft: '', temperature: 0.7 };
let restored: Partial<typeof defaults> = {};
try { const saved = JSON.parse(native.StateJson); if (saved.kind === 'chat' && saved.schema === 1) restored = saved.data; } catch {}
export const state = reactive({ ...defaults, ...restored, endpoint: 'http://127.0.0.1:4180/v1/chat/completions', model: 'local-demo', protocol: 'openai',
  generating: false, ready: false, settings: false, sidebar: false, toast: '', error: '', streamId: '', follow: true, editing: -1, deltaCount: 0 });
for (const conversation of state.conversations) for (const message of conversation.messages) if (message.status === 'streaming') message.status = 'interrupted';
export function current(): Conversation | undefined { return state.conversations.find(c => c.id === state.activeId); }
function lastMessage() { const messages = current()?.messages; return messages?.[messages.length - 1]; }
export function serialize(): string {
  return JSON.stringify({ schema: 1, kind: 'chat', data: { conversations: state.conversations, activeId: state.activeId, nextId: state.nextId, draft: state.draft, temperature: state.temperature } });
}
export async function persist() { try { await callHostJson('chat.persist', JSON.parse(serialize())); } catch (error) { state.error = String(error); } }
export function notify(text: string) { state.toast = text; setTimeout(() => { state.toast = ''; }, 1800); }
export async function initialize() {
  try {
    Object.assign(state, await callHostJson('chat.config', {}));
    if (!state.conversations.length) {
      const saved: any = await callHostJson('chat.load', {});
      if (saved.kind === 'chat' && saved.schema === 1 && Array.isArray(saved.data?.conversations)) Object.assign(state, saved.data);
      for (const c of state.conversations) for (const m of c.messages) if (m.status === 'streaming') m.status = 'interrupted';
    }
    if (!state.conversations.length) newConversation();
    state.ready = true;
    focusComposer();
  } catch (error) { state.error = String(error); }
}
export function focusComposer() { requestAnimationFrame(() => { const id = native.FindNode('chat-input'); if (id) native.FocusNode(id); }); }
export function scrollBottom() {
  requestAnimationFrame(() => { const id = native.FindNode('chat-scroll'); if (id) native.ScrollNode(id, -1); });
}
export function newConversation() {
  stop();
  const id = state.nextId++;
  state.conversations.unshift({ id, title: 'New conversation', messages: [] }); state.activeId = id; state.draft = ''; state.editing = -1;
  state.sidebar = false; state.error = ''; state.follow = true;
  if (state.conversations.length > 20) state.conversations.pop();
  focusComposer();
}
export function selectConversation(id: number) { stop(); state.activeId = id; state.draft = ''; state.sidebar = false; state.editing = -1; state.follow = true; scrollBottom(); }
export function deleteConversation(id: number) {
  if (state.activeId === id) stop();
  state.conversations = state.conversations.filter(c => c.id !== id);
  if (state.activeId === id) state.activeId = state.conversations[0]?.id || 0;
  if (!state.activeId) newConversation(); void persist();
}
export function stop() {
  if (!state.generating) return;
  const message = lastMessage(); if (message) message.status = 'stopped';
  state.generating = false; state.streamId = ''; void callHostJson('chat.stop', {}); void persist();
}
export async function send() {
  const content = state.draft.trim();
  if (!content || state.generating || !state.ready) return;
  if (content.length > 8000) { state.error = 'Message exceeds 8,000 characters.'; return; }
  const conversation = current(); if (!conversation) return;
  if (state.editing >= 0) conversation.messages = conversation.messages.slice(0, state.editing);
  if (!conversation.messages.length) conversation.title = content.slice(0, 34);
  conversation.messages.push({ id: state.nextId++, role: 'user', content });
  state.draft = ''; state.editing = -1;
  await generate();
}
export async function generate() {
  if (state.generating) return;
  const conversation = current(); if (!conversation || !conversation.messages.length) return;
  state.generating = true; state.error = ''; state.follow = true; state.deltaCount = 0;
  const message = reactive<Message>({ id: state.nextId++, role: 'assistant', content: '', status: 'streaming' });
  const history = conversation.messages.filter(m => m.content).map(m => ({ role: m.role, content: m.content }));
  conversation.messages.push(message);
  const streamId = `${conversation.id}-${message.id}-${Date.now()}`; state.streamId = streamId;
  scrollBottom();
  try { await callHostJson('chat.start', { streamId, messages: history, temperature: state.temperature }); }
  catch (error) { if (state.streamId === streamId) { message.status = 'error'; message.error = String(error); state.error = String(error); state.generating = false; } }
}
export function edit(index: number) { stop(); const message = current()?.messages[index]; if (!message) return; state.editing = index; state.draft = message.content; focusComposer(); }
export function regenerate(index: number) { stop(); const conversation = current(); if (!conversation) return; conversation.messages = conversation.messages.slice(0, index); void generate(); }
export async function copyMessage(content: string) { await callHostJson('chat.copy', { text: content }); notify('Copied'); }
export async function configure() {
  stop();
  try { await callHostJson('chat.configure', { endpoint: state.endpoint, model: state.model, protocol: state.protocol }); state.settings = false; state.error = ''; }
  catch (error) { state.error = String(error); }
}
function stream(json: string) {
  const event = JSON.parse(json);
  if (event.type !== 'chat.stream' || event.streamId !== state.streamId || !state.generating) return;
  const message = lastMessage(); if (!message) return;
  const scroll = native.FindNode('chat-scroll');
  if (scroll && state.follow && native.ScrollRemaining(scroll) > 90) state.follow = false;
  message.content += event.delta;
  if (event.delta) state.deltaCount++;
  if (message.content.length > 65536) { stop(); state.error = 'Response exceeds 64 KiB of text.'; return; }
  message.status = event.status; message.error = event.error;
  if (state.follow) scrollBottom();
  if (event.status !== 'streaming') { state.generating = false; if (event.error) state.error = event.error; void persist(); }
}
native.OnHostEvent.Add(stream);
export function disposeChat() { native.OnHostEvent.Remove(stream); }
watchEffect(() => native.ReportDebugState(JSON.stringify({ ...state, messages: current()?.messages || [] })));
