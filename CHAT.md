# Native Vue Markdown Chat

This example renders a nanochat-inspired conversation UI in Unreal Engine through Vue's custom renderer and RmlUi. It uses **markdown-it 15.0.1** and **highlight.js 11.12.0**, with no browser widget or runtime DOM. The existing native Grid implementation lays out the shell, composer, lists and Markdown tables.

The visual reference is nanochat's [historical UI](https://github.com/karpathy/nanochat/blob/21a7774eab3977a9e0629a2b47eb237cafe54a80/nanochat/ui.html): restrained header, centered conversation column, right-aligned user bubbles, unframed assistant messages and bottom composer. The current upstream removed that UI; this example pins the earlier reference and reimplements the appearance in RCSS.

## Launch

From the project root, after building the UE modules:

```powershell
.\LaunchChat.ps1 -Watch
```

Without an endpoint this starts a localhost deterministic SSE fixture and opens the interactive UE game window. The fixture returns predefined Markdown, not model-generated answers. `-Watch` starts a hidden source watcher; its PID is printed and its logs are in `Saved/ChatWatch*.log`. Editing `Frontend/src/chat` publishes a new version which UE adopts automatically. `BuildChat.ps1 -Watch` can also run the watcher in a terminal.

Connect your own OpenAI-compatible Chat Completions server:

```powershell
.\LaunchChat.ps1 -Endpoint 'http://127.0.0.1:8000/v1/chat/completions' -Model 'your-model' -Watch
```

For nanochat's historical web server protocol:

```powershell
.\LaunchChat.ps1 -Endpoint 'http://127.0.0.1:8000/chat/completions' -Protocol nanochat
```

For authentication set `RMLUI_CHAT_API_KEY` in the launching process environment. The UE host reads it and passes it directly to the transport; it is not exposed to Vue or stored in conversation history. Settings allow changing endpoint, model, protocol and temperature. Use HTTPS for non-localhost endpoints. These are Chat Completions contracts, not the OpenAI Responses API, tool calls or multimodal content.

The packaged executable accepts `-Chat`, `-ChatEndpoint=...`, `-ChatModel=...` and `-ChatProtocol=openai|nanochat`. It does not require Node for UI execution; a real model service, or the optional Node fixture, supplies responses separately.

## Reuse

Import `Frontend/src/chat/MarkdownView.ts` as a normal Vue component:

```vue
<script setup lang="ts">
import RmlMarkdown from './chat/MarkdownView';
defineProps<{ text: string; id: number }>();
</script>
<template>
  <RmlMarkdown :content="text" :message-id="id" />
</template>
```

`markdown.ts` converts the official parser's tokens to a whitelisted node tree. `MarkdownView.ts` renders that tree using Vue VNodes. highlight.js output is parsed with htmlparser2 and reduced to spans and text. No untrusted HTML is inserted with `innerHTML` or `v-html`. Include the Markdown RCSS rules from `ChatApp.vue`, icon assets, fonts and the `chat.copy` / `chat.openLink` host handlers when moving the component elsewhere.

For the complete chat, retain both `URmlUiJSRuntime` and `URmlUiChatTransport` as UPROPERTY references. Call `Transport->Attach(Runtime, Endpoint, Model, Protocol)` before `Runtime->Start`, with `Content/Chat/current.json` as the manifest; call `SetApiKey` from native code if required. Stop the transport and runtime when closing the screen. The test project's `RmlUiDemoGameMode.cpp` is the working host example. Copy the unified `RmlUiUnreal` plugin and its required `Puerts` plugin dependency for another UE host.

## Supported Surface

| Area | Implemented |
| --- | --- |
| Markdown | Headings, paragraphs, bold, italic, strikeout, links, quotations, rules, line breaks, ordered and nested lists, fenced and inline code, tables |
| Highlighting | C++, JavaScript, TypeScript, Python and JSON; unknown languages fall back to plain code |
| Rendering | Chinese font fallback, syntax colors, raw code copy, native vertical and horizontal scrolling, desktop and narrow window layouts |
| Conversation | New/select/delete history, user message editing, regenerate, copy, Enter to send and Shift+Enter for a new line |
| Streaming | Native UE HTTP, OpenAI delta.content / [DONE], nanochat token / done, split UTF-8 and SSE lines, CRLF, comments, cancellation and bounded buffers |
| Failures | HTTP failure, invalid SSE JSON, truncated streams, request timeout and size limits preserve messages and display an error |
| Reload | Completed conversation and draft state survive VM replacement; an active response is restored as interrupted and its old HTTP request is cancelled |
| Persistence | Completed/stopped conversations are written to the host's Saved/RmlUiChat/session.json using a temporary file and rename |

The adapter is a reusable Vue Markdown component powered by mainstream packages. Existing browser Markdown components that manipulate DOM, CSS variables or browser selection are not drop-in compatible. KaTeX, Mermaid, task checkboxes, remote images, file uploads, tool results, rich text selection and native Chinese IME composition are not implemented here. Chinese glyph rendering and streamed Unicode are verified separately from IME input. Only the bundled `hello_world.png` is accepted as a Markdown image; other images display alt text. Links are restricted to HTTP(S), and raw HTML remains literal text.

The active response is reparsed on each batched update, with keyed Vue patches retaining completed prefix nodes. This is not an incremental Markdown parser or a virtualized transcript. Current limits are 8,000 input characters, 65,536 response UTF-16 code units, 100 messages per request, 262,144 total request code units, 4 MiB of SSE bytes and 20 conversations. Long-history performance has not been benchmarked. The underlying renderer still uses the existing DX11 readback/upload bridge.

RmlUi requires explicit scrollbar dimensions; missing styles can consume the content width. Its intrinsic sizing also differs for percentage max-width on shrink-to-fit bubbles. The chat supplies scrollbars and pixel width caps at responsive breakpoints. No changes to the user's Grid solver were required.

## Dependencies

The lockfile pins all npm dependencies. Lucide icons are rasterized into PNGs during the build, and font/icon/package licenses are included in each version's manifest. Noto Sans CJK is from [notofonts/noto-cjk](https://github.com/notofonts/noto-cjk), JetBrains Mono from [Google Fonts](https://github.com/google/fonts/tree/main/ofl/jetbrainsmono), and the Lato italic faces from the existing vendored RmlUi assets. Build-time Babel transforms Unicode property regexes because the bundled Puerts V8 does not enable that regex capability; real UE execution verifies this compatibility path.

Official sources: [markdown-it](https://github.com/markdown-it/markdown-it), [highlight.js](https://github.com/highlightjs/highlight.js), [Vue custom renderer](https://vuejs.org/api/custom-renderer.html), [nanochat server reference](https://github.com/karpathy/nanochat/blob/21a7774eab3977a9e0629a2b47eb237cafe54a80/scripts/chat_web.py).

See the project's `CHAT_VALIDATION.md` for commands, measured results and actual UE captures.
