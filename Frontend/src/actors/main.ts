import '../runtime';
import { createApp, disposeRenderer } from '@rmlui/vue';
import { disposeRuntime } from '../runtime';
import { native, report } from '../bridge';
import ActorObserverApp from './ActorObserverApp.vue';

try {
  const instance = createApp(ActorObserverApp);
  instance.mount();
  native.OnLifecycle.Add(action => {
    if (action === 'dispose') { instance.unmount(); disposeRuntime(); disposeRenderer(); }
  });
  native.ReportReady();
} catch (error) { report(error); }
