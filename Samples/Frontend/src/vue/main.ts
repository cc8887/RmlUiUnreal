import '../../../../Frontend/src/runtime';
import { createApp, disposeRenderer } from '@rmlui/vue';
import { disposeRuntime } from '../../../../Frontend/src/runtime';
import { native, report } from '../../../../Frontend/src/bridge';
import { serialize } from './store';
import App from './App.vue';

try {
  const instance = createApp(App);
  instance.mount();
  native.OnLifecycle.Add(action => {
    if (action === 'serialize') native.SaveState(serialize());
    if (action === 'dispose') { instance.unmount(); disposeRuntime(); disposeRenderer(); }
  });
  native.ReportReady();
} catch (error) { report(error); }
