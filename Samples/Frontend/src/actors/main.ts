import '../../../../Frontend/src/runtime';
import { createApp, disposeRenderer } from '@rmlui/vue';
import { disposeRuntime } from '../../../../Frontend/src/runtime';
import { native, report } from '../../../../Frontend/src/bridge';
import ActorObserverApp from './ActorObserverApp.vue';
import { serialize as serializeChat, disposeChat } from '../chat/store';
import { serialize as serializeVue } from '../vue/store';

try {
  const instance = createApp(ActorObserverApp);
  instance.mount();
  native.OnLifecycle.Add(action => {
    if (action === 'serialize') native.SaveState(JSON.stringify({ schema: 1, kind: 'demo',
      chat: JSON.parse(serializeChat()).data, vue: JSON.parse(serializeVue()).data }));
    if (action === 'dispose') { disposeChat(); instance.unmount(); disposeRuntime(); disposeRenderer(); }
  });
  native.ReportReady();
} catch (error) { report(error); }
