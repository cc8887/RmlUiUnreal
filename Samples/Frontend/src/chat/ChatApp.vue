<script setup lang="ts">
import { computed, onMounted, ref } from '@rmlui/vue';
import type { RmlEvent } from '../../../../Frontend/src/renderer';
import { native } from '../../../../Frontend/src/bridge';
import MarkdownView from './MarkdownView';
import { state, current, initialize, newConversation, selectConversation, deleteConversation, send, stop, edit, regenerate, copyMessage, configure, scrollBottom } from './store';
const conversation = computed(current);
const messages = computed(() => conversation.value?.messages || []);
const hover = ref('');
function tooltip(event: RmlEvent) {
  hover.value = '';
  for (let node = event.target.handle; node; node = native.ParentNode(node)) {
    const title = native.GetAttribute(node, 'title');
    if (title) { hover.value = title; break; }
  }
}
onMounted(initialize);
function key(event: RmlEvent) {
  if (event.keyName === 'Enter' && !(event.modifiers & 1)) {
    event.stopImmediatePropagation(); event.suppressTextInput(); void send();
  }
}
function prompt(text: string) { state.draft = text; void send(); }
const suggestions = ['Explain CSS Grid', 'Write a C++ function', 'Compare rendering options'];
</script>

<template>
  <div id="chat-app" @mouseover.capture="tooltip" @mouseout.capture="hover = ''">
    <div id="chat-header">
      <div class="header-left">
        <button id="chat-history" class="icon-button" @click="state.sidebar = !state.sidebar" @mouseenter="hover = 'Conversations'" @mouseleave="hover = ''"><img src="icons/panel-left.png" /></button>
        <button id="chat-new" class="icon-button" @click="newConversation" @mouseenter="hover = 'New conversation'" @mouseleave="hover = ''"><img src="icons/plus.png" /></button>
        <h1>nanochat</h1><span class="engine-label">UNREAL</span>
      </div>
      <div class="header-right"><span id="chat-model" class="model-label">{{ state.model }}</span><button id="chat-settings" class="icon-button" @click="state.settings = !state.settings" @mouseenter="hover = 'Model settings'" @mouseleave="hover = ''"><img src="icons/settings-2.png" /></button></div>
    </div>
    <div v-if="hover" id="chat-tooltip">{{ hover }}</div>
    <div id="chat-scroll" @wheel="state.follow = false">
      <div id="chat-thread">
        <div v-if="!messages.length" id="chat-empty">
          <img class="empty-mark" src="icons/messages-square.png" />
          <h2>What are we working on?</h2>
          <div class="suggestions"><button v-for="(suggestion, index) in suggestions" :key="index" :id="'chat-suggestion-' + index" @click="prompt(suggestion)">{{ suggestion }}</button></div>
        </div>
        <div v-for="(message, index) in messages" :key="message.id" :id="'chat-message-' + message.id" :class="['message', message.role]">
          <div v-if="message.role === 'user'" class="user-content"><p>{{ message.content }}</p><button :id="'chat-edit-' + message.id" class="icon-button message-edit" title="Edit message" @click="edit(index)"><img src="icons/pencil.png" /></button></div>
          <div v-else class="assistant-content">
            <div class="assistant-heading"><span class="assistant-mark">n</span><span>nanochat</span><span v-if="message.status === 'streaming'" class="stream-state">Generating</span><span v-if="message.status === 'stopped' || message.status === 'interrupted'" class="stream-state">Stopped</span></div>
            <MarkdownView :content="message.content" :message-id="message.id" />
            <div v-if="message.status === 'streaming' && !message.content" class="thinking">Thinking...</div>
            <div v-if="message.error" class="message-error">{{ message.error }}</div>
            <div v-if="message.status !== 'streaming'" class="message-actions">
              <button :id="'chat-copy-' + message.id" class="icon-button" title="Copy message" @click="copyMessage(message.content)"><img src="icons/copy.png" /></button>
              <button :id="'chat-retry-' + message.id" class="icon-button" title="Regenerate" @click="regenerate(index)"><img src="icons/rotate-ccw.png" /></button>
            </div>
          </div>
        </div>
      </div>
    </div>
    <div id="chat-composer-band">
      <div id="chat-composer">
        <div v-if="state.error" id="chat-error">{{ state.error }}</div>
        <div v-if="state.editing >= 0" class="editing-row"><span>Editing message</span><button id="chat-cancel-edit" class="icon-button" title="Cancel editing" @click="state.editing = -1; state.draft = ''"><img src="icons/x.png" /></button></div>
        <div class="composer-row">
          <textarea id="chat-input" rows="2" placeholder="Ask anything" v-model="state.draft" @keydown="key" />
          <button v-if="state.generating" id="chat-stop" class="send-button" title="Stop generating" @click="stop"><img src="icons/square-white.png" /></button>
          <button v-else id="chat-send" class="send-button" title="Send message" :disabled="!state.draft.trim() || !state.ready" @click="send"><img src="icons/arrow-up-white.png" /></button>
        </div>
        <div class="composer-footer"><span>{{ state.protocol === 'nanochat' ? 'nanochat' : 'Chat Completions' }}</span><span id="chat-status">{{ state.generating ? 'Generating' : (state.ready ? 'Ready' : 'Connecting') }}</span></div>
      </div>
    </div>
    <button v-if="!state.follow && messages.length" id="chat-bottom" class="icon-button jump-bottom" title="Jump to latest" @click="state.follow = true; scrollBottom()"><img src="icons/arrow-down.png" /></button>
    <div v-if="state.sidebar" id="chat-sidebar">
      <div class="sidebar-title"><h2>Conversations</h2><button class="icon-button" title="Close conversations" @click="state.sidebar = false"><img src="icons/x.png" /></button></div>
      <div v-for="item in state.conversations" :key="item.id" :class="['conversation-row', item.id === state.activeId ? 'selected' : '']">
        <button :id="'chat-select-' + item.id" class="conversation-name" @click="selectConversation(item.id)">{{ item.title }}</button><button :id="'chat-delete-' + item.id" class="icon-button" title="Delete conversation" @click="deleteConversation(item.id)"><img src="icons/trash-2.png" /></button>
      </div>
    </div>
    <div v-if="state.settings" id="chat-settings-overlay">
      <div id="chat-settings-dialog">
        <div class="sidebar-title"><h2>Model settings</h2><button id="chat-close-settings" class="icon-button" title="Close settings" @click="state.settings = false"><img src="icons/x.png" /></button></div>
        <label>Protocol</label><select id="chat-protocol" v-model="state.protocol"><option value="openai">OpenAI compatible</option><option value="nanochat">nanochat</option></select>
        <label>Endpoint</label><input id="chat-endpoint" type="text" v-model="state.endpoint" />
        <label>Model</label><input id="chat-model-input" type="text" v-model="state.model" />
        <label>Temperature: {{ state.temperature }}</label><input id="chat-temperature" type="range" min="0" max="2" step="0.1" v-model.number="state.temperature" />
        <button id="chat-apply-settings" class="apply-button" @click="configure">Apply</button>
      </div>
    </div>
    <div v-if="state.toast" id="chat-toast">{{ state.toast }}</div>
  </div>
</template>

<style>
body { margin:0; width:100%; height:100%; overflow:hidden; background-color:#ffffff; color:#202124; font-family:LatoLatin; font-size:16px; }
div,p,h1,h2,h3,h4,h5,h6,blockquote,pre { display:block; box-sizing:border-box; }
h1,h2,h3,h4,p { margin:0; } h1 { font-size:23px; } h2 { font-size:22px; } button,input,textarea,select { font-family:LatoLatin; }
#chat-app { display:grid; grid-template-columns:minmax(0px,1fr); grid-template-rows:68px minmax(0px,1fr) auto; width:100%; height:100%; }
#chat-header { display:flex; justify-content:space-between; align-items:center; padding:0 24px; border-bottom:1px #eff0f1; }
.header-left,.header-right { display:flex; align-items:center; gap:10px; } .header-left h1 { margin-left:6px; }
.engine-label { color:#7c858c; font-size:10px; margin-left:3px; } .model-label { color:#73777c; font-size:13px; }
.icon-button { display:flex; align-items:center; justify-content:center; width:32px; height:32px; padding:0; background-color:transparent; border-radius:5px; }
.icon-button img { width:17px; height:17px; } .icon-button:hover { background-color:#edf0f2; } button:disabled { opacity:0.4; }
scrollbarvertical { width:10px; } scrollbarvertical slidertrack { background-color:#fafbfc; }
scrollbarvertical sliderbar { width:6px; min-height:28px; margin:0 2px; background-color:#c6cdd4; border-radius:3px; }
scrollbarvertical sliderarrowdec,scrollbarvertical sliderarrowinc { height:0; }
scrollbarhorizontal { height:10px; } scrollbarhorizontal slidertrack { background-color:#edf0f4; }
scrollbarhorizontal sliderbar { height:6px; min-width:28px; margin:2px 0; background-color:#bcc7d1; border-radius:3px; }
scrollbarhorizontal sliderarrowdec,scrollbarhorizontal sliderarrowinc { width:0; }
#chat-scroll { width:100%; overflow-y:auto; overflow-x:hidden; min-height:0; }
#chat-thread { width:100%; max-width:816px; margin:0 auto; padding:24px 24px 36px; }
#chat-empty { padding-top:90px; text-align:center; } .empty-mark { width:36px; height:36px; margin-bottom:24px; opacity:0.7; }
#chat-empty h2 { font-size:27px; margin-bottom:28px; }
.suggestions { display:flex; justify-content:center; flex-wrap:wrap; gap:10px; }
.suggestions button { padding:11px 14px; background-color:#f7f8fa; border:1px #e7e9ec; color:#555c65; font-size:13px; border-radius:6px; }
.suggestions button:hover { background-color:#eef2f7; }
.message { margin-bottom:24px; width:100%; } .message.user { text-align:right; }
.user-content { display:inline-block; text-align:left; position:relative; max-width:576px; background-color:#f2f3f5; border-radius:8px; padding:13px 42px 13px 18px; }
.user-content p { white-space:pre-wrap; line-height:1.6; word-break:break-word; }
.message-edit { position:absolute; right:5px; top:8px; opacity:0.55; }
.assistant-content { width:100%; min-width:0; } .assistant-heading { display:flex; align-items:center; gap:9px; font-size:13px; color:#444b53; margin:0 0 14px; }
.assistant-mark { display:inline-block; background-color:#eef4f2; color:#18775c; width:24px; height:24px; text-align:center; line-height:24px; border-radius:4px; }
.stream-state { color:#818b93; font-size:12px; margin-left:6px; }
.message-actions { display:flex; gap:4px; margin-top:10px; } .thinking { color:#848b92; padding:8px 0; }
#chat-composer-band { padding:12px 24px 14px; border-top:1px #f0f1f2; background-color:#ffffff; }
#chat-composer { width:100%; max-width:768px; margin:0 auto; }
.composer-row { display:grid; grid-template-columns:minmax(0px,1fr) 50px; gap:10px; align-items:end; }
#chat-input { display:block; width:100%; height:78px; box-sizing:border-box; padding:12px 15px; border:1px #ced3d9; border-radius:8px; background-color:#ffffff; color:#22262b; font-size:16px; line-height:1.5; }
#chat-input:focus { border-color:#517db5; }
.send-button { display:flex; align-items:center; justify-content:center; width:50px; height:50px; background-color:#242931; border-radius:8px; }
.send-button img { width:22px; height:22px; } .send-button:hover { background-color:#3469a4; }
.composer-footer { display:flex; justify-content:space-between; color:#8a9098; font-size:11px; padding-top:10px; }
.editing-row { display:flex; justify-content:space-between; align-items:center; font-size:12px; color:#497195; padding-bottom:6px; }
.message-error,#chat-error { color:#a33b3b; background-color:#fff2f1; border-left:3px #cc7772; padding:9px 12px; margin-bottom:10px; font-size:13px; }
.jump-bottom { position:absolute; right:28px; bottom:158px; background-color:#ffffff; border:1px #dce0e3; }
#chat-sidebar { position:absolute; top:68px; bottom:0; left:0; width:275px; padding:18px 12px; background-color:#f8f9fa; border-right:1px #e0e4e8; overflow-y:auto; z-index:4; }
.sidebar-title { display:flex; align-items:center; justify-content:space-between; margin-bottom:18px; } .sidebar-title h2 { font-size:17px; }
.conversation-row { display:flex; align-items:center; margin-bottom:4px; border-radius:5px; } .conversation-row.selected { background-color:#e9edf1; }
.conversation-name { display:block; flex:1; min-width:0; padding:10px 8px; color:#454b53; font-size:13px; text-align:left; }
#chat-settings-overlay { position:absolute; top:68px; right:0; bottom:0; left:0; background-color:#20212440; z-index:6; }
#chat-settings-dialog { width:400px; max-width:92%; margin:26px auto 0; background-color:#ffffff; border:1px #dce0e3; padding:22px; border-radius:8px; }
#chat-settings-dialog label { display:block; color:#5e6873; font-size:13px; margin:16px 0 7px; }
#chat-settings-dialog input[type=text],#chat-settings-dialog select { display:block; width:100%; height:36px; box-sizing:border-box; padding:8px; border:1px #cdd4db; border-radius:4px; color:#303640; background-color:#ffffff; font-size:13px; }
select selectvalue { width:100%; } select selectarrow { width:14px; background-color:#edf0f3; } select selectbox { background-color:#ffffff; border:1px #cdd4db; }
select option { display:block; padding:8px; } select option:hover,select option:checked { background-color:#e8f0f9; }
#chat-temperature { display:block; width:100%; height:24px; } #chat-temperature slidertrack { height:5px; margin-top:9px; background-color:#e0e5ec; }
#chat-temperature sliderbar { width:12px; height:20px; background-color:#477cad; } #chat-temperature sliderprogress { height:5px; background-color:#477cad; }
#chat-temperature sliderarrowdec,#chat-temperature sliderarrowinc { width:0; }
.apply-button { display:block; background-color:#2d547f; color:#ffffff; padding:10px 18px; margin-top:22px; border-radius:5px; }
#chat-toast { position:absolute; right:24px; top:78px; padding:9px 14px; color:#ffffff; background-color:#305e4b; font-size:13px; border-radius:5px; z-index:9; }
#chat-tooltip { position:absolute; top:58px; left:24px; padding:6px 9px; background-color:#333a43; color:#ffffff; font-size:12px; z-index:10; border-radius:4px; }
.markdown { line-height:1.65; color:#25282d; width:100%; min-width:0; }
.markdown p { margin:0 0 14px; word-break:break-word; } .markdown h1 { font-size:25px; margin:20px 0 12px; }
.markdown h2 { font-size:21px; margin:18px 0 10px; } .markdown h3 { font-size:18px; margin:16px 0 8px; }
.markdown h4,.markdown h5,.markdown h6 { font-size:16px; margin:14px 0 8px; }
.markdown strong { font-weight:bold; } .markdown em { font-style:italic; } .markdown s { text-decoration:line-through; }
.markdown blockquote { border-left:3px #b4c6d9; padding:4px 0 4px 16px; margin:14px 0; color:#5c6e80; }
.md-link { color:#326da7; text-decoration:underline; } .md-inline-code { font-family:JetBrains Mono; font-size:13px; background-color:#edf0f4; color:#795877; padding:2px 4px; }
.md-ul,.md-ol { margin:8px 0 14px; } .md-li { display:grid; grid-template-columns:22px minmax(0px,1fr); gap:4px; margin-bottom:5px; }
.md-li-body p { margin-bottom:6px; } .md-marker { color:#657486; }
.md-code-block { margin:14px 0 18px; border:1px #dfe4e9; border-radius:6px; overflow:hidden; }
.md-code-header { display:flex; justify-content:space-between; align-items:center; padding:5px 10px 5px 14px; background-color:#eef1f5; color:#6a7785; font-size:12px; border-bottom:1px #dfe4e9; }
.code-copy { width:26px; height:26px; } .code-copy img { width:14px; height:14px; }
.markdown pre { margin:0; padding:16px; background-color:#f8fafc; font-family:JetBrains Mono; font-size:13px; line-height:1.65; white-space:pre; overflow-x:auto; color:#2e3b4c; }
.hljs-keyword,.hljs-selector-tag,.hljs-literal { color:#85539e; } .hljs-string,.hljs-attr { color:#25724c; } .hljs-number,.hljs-built_in { color:#b86624; }
.hljs-title,.hljs-function { color:#286fb0; } .hljs-comment { color:#7c8c99; } .hljs-type { color:#2b8285; }
.md-table { overflow-x:auto; margin:16px 0; border:1px #dfe4e9; border-radius:4px; }
.md-tr { display:grid; width:100%; } .md-th,.md-td { padding:9px 11px; border-bottom:1px #e7ebef; font-size:14px; }
.md-th { background-color:#edf2f7; color:#345370; font-weight:bold; } .md-td { background-color:#ffffff; }
.md-rule { height:1px; background-color:#e0e4e8; margin:22px 0; } .md-image { display:block; width:200px; max-width:100%; margin:12px 0; }
.md-image-label { color:#7d8792; }
@media (max-width:600px) {
  #chat-header { padding:0 12px; } .header-left,.header-right { gap:4px; } .engine-label { display:none; } .model-label { max-width:90px; font-size:11px; }
  #chat-thread { padding:18px 16px 28px; } #chat-composer-band { padding:10px 12px 12px; }
  #chat-empty { padding-top:70px; } #chat-empty h2 { font-size:22px; } .suggestions { display:block; } .suggestions button { display:block; margin:8px auto; }
  .user-content { max-width:300px; } .markdown { font-size:15px; } .markdown pre { font-size:12px; padding:12px; }
  .composer-row { grid-template-columns:minmax(0px,1fr) 44px; gap:8px; } .send-button { width:44px; height:44px; }
  #chat-sidebar { width:260px; } .jump-bottom { right:14px; }
}
@media (max-width:400px) { .user-content { max-width:260px; } }
@media (max-width:350px) { .user-content { max-width:220px; } }
</style>
