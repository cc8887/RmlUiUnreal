import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { buildFrontend, watchFrontend } from '../../../Frontend/tools/build.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const core = path.resolve(root, '../../Frontend');
const content = path.resolve(root, '../../Content/RmlUi');
const commonFonts = ['NotoSansCJKsc-Regular.otf', 'JetBrainsMono.ttf'];
const actorIcons = ['panel-bottom', 'pause', 'play', 'list-tree', 'panels-top-left', 'git-branch',
  'zoom-in', 'zoom-out', 'maximize-2', 'search', 'circle-plus', 'eye', 'settings-2', 'save',
  'trash-2', 'app-window', 'triangle-alert', 'panel-right-open', 'x', 'sparkles', 'rotate-ccw', 'chevron-down'];
const chatIcons = ['arrow-up', 'arrow-down', 'square', 'copy', 'check', 'plus', 'pencil',
  'rotate-ccw', 'settings-2', 'panel-left', 'x', 'trash-2', 'messages-square'];

export async function buildSample({ app = 'vue', outputRoot, mirrorRoot, activate = true } = {}) {
  if (!['vue', 'chat', 'actors'].includes(app)) throw new Error(`Unknown sample app: ${app}`);
  const isActor = app === 'actors', isChat = app === 'chat';
  const bundleName = isActor ? 'ActorObserver' : isChat ? 'Chat' : 'Vue';
  const assetFiles = [{ name: 'hello_world.png', source: path.join(content, 'hello_world.png') }];
  if (isActor || isChat) {
    for (const name of ['OFL-Noto.txt', 'OFL-JetBrains.txt'])
      assetFiles.push({ name: `fonts/${name}`, source: path.join(root, 'assets', name) });
    assetFiles.push({ name: 'fonts/OFL-Lato.txt', source: path.join(content, 'Fonts/LICENSE.txt') });
  }
  return buildFrontend({
    entryPoint: path.join(root, 'src', isActor ? 'actors/main.ts' : isChat ? 'chat/main.ts' : 'vue/main.ts'),
    outputRoot: outputRoot ?? path.join(root, '../RmlUiUnrealSamples/Content', bundleName),
    mirrorRoot, activate,
    profile: isActor ? 'slate-rhi' : 'dx11-compat',
    mode: isActor ? 'degrade' : 'strict',
    // Existing shadow specimens are intentionally degraded on the Slate showcase.
    allowDegrade: isActor ? ['render.layers', 'render.filters'] : [],
    requiredFeatures: isActor ? ['nodes.query', 'layout.measure', 'events.extended', 'overlays.modal', 'input.ime'] : [],
    tailwindContentFile: isActor ? path.join(root, 'src/actors/ActorObserverApp.vue') : undefined,
    tailwindTheme: isActor ? { extend: { colors: { ink: '#202a2e', signal: '#14866d', warning: '#c27a28' } } } : undefined,
    rewriteUnicodeProperties: isChat,
    requiredMotionRule: rule => isActor &&
      (rule.selector.startsWith('.css-motion-run.css-motion-item-') || rule.selector === '.css-control-loop'),
    dependencyRoots: [path.join(root, 'node_modules'), path.join(core, 'node_modules')],
    iconNames: isActor ? actorIcons : isChat ? chatIcons : [],
    iconDirectory: path.join(root, 'node_modules/lucide-static/icons'),
    fontFiles: isActor ? commonFonts : isChat ? [...commonFonts, 'LatoLatin-Italic.ttf', 'LatoLatin-BoldItalic.ttf'] : [],
    fontDirectory: path.join(root, 'assets'), assetFiles,
    watchRoots: [path.join(core, 'src'), path.join(root, 'src')],
  });
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  if (process.argv.includes('--actors') && process.argv.includes('--chat'))
    throw new Error('Choose one sample app per build.');
  const app = process.argv.includes('--actors') ? 'actors' : process.argv.includes('--chat') ? 'chat' : 'vue';
  const mirrorIndex = process.argv.indexOf('--mirror-output');
  if (mirrorIndex >= 0 && !process.argv[mirrorIndex + 1]) throw new Error('--mirror-output requires a directory.');
  const options = { app, mirrorRoot: mirrorIndex >= 0 ? process.argv[mirrorIndex + 1] : undefined };
  await buildSample(options);
  if (process.argv.includes('--watch')) {
    watchFrontend({ watchRoots: [path.join(core, 'src'), path.join(root, 'src')],
      build: () => buildSample(options) });
  }
}
