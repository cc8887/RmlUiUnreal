import '../../../../Frontend/src/runtime';
import { createApp, disposeRenderer } from '@rmlui/vue';
import { disposeRuntime } from '../../../../Frontend/src/runtime';
import { native, report } from '../../../../Frontend/src/bridge';
import { serialize, disposeChat } from './store';
import ChatApp from './ChatApp.vue';

try {
  const instance = createApp(ChatApp); instance.mount();
  native.OnLifecycle.Add(action => {
    if (action === 'serialize') native.SaveState(serialize());
    if (action === 'dispose') { disposeChat(); instance.unmount(); disposeRuntime(); disposeRenderer(); }
  });
  native.ReportReady();
} catch (error) { report(error); }
