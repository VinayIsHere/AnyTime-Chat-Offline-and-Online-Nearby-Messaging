import { useCallback, useEffect, useRef, useState } from 'react';

import { api, type RadioAvailability } from '../api/client';
import { onEvent } from '../api/events';

/**
 * Single source of truth for radio state.
 *
 * NO NATIVE JAVASCRIPT BRIDGE. The handed-over design assumed a
 * `window.AnyTimeBluetooth` injected by a Capacitor/React-Native shell, but
 * this app has no such shell: the page authenticates by knowing its secret URL
 * and reaches the core over loopback HTTP (AD-03 / AD-08). AnyTimeActivity
 * installs no bridge on purpose — one would be a second, weaker path into the
 * core — so the same contract is served by the local API instead.
 *
 * SSE is the TRIGGER, HTTP is the SOURCE OF TRUTH. The `radio.state` event says
 * a transport came up or went down; it cannot distinguish "switched off" from
 * "permission not granted", which is the whole difference the gate screen has
 * to explain. So the event prompts a refetch rather than being believed
 * directly — the same refetch-on-signal shape the Nearby screen uses.
 */
export function useBluetooth() {
  const [state, setState] = useState<RadioAvailability>('unknown');
  const [canPromptInApp, setCanPromptInApp] = useState(false);
  const [requesting, setRequesting] = useState(false);
  const [denied, setDenied] = useState(false);

  const deniedTimer = useRef<number | undefined>(undefined);

  const refresh = useCallback(async () => {
    try {
      const status = await api.getBluetooth();
      setState(status.state);
      setCanPromptInApp(status.canPromptInApp);
      return status.state;
    } catch {
      // The core is not answering. App already shows its own "could not start"
      // screen for that, and stranding the user behind a Bluetooth gate they
      // cannot clear would be the wrong failure — treat it as nothing to gate.
      setState('unsupported');
      return 'unsupported' as RadioAvailability;
    }
  }, []);

  useEffect(() => {
    void refresh();

    // AppCore retries every transport every 3 s while any is down, so flipping
    // Bluetooth on — here, or from the notification shade — surfaces as this
    // event within a few seconds without anything polling.
    const offRadio = onEvent('radio.state', () => { void refresh(); });

    // Returning from the system dialog or the settings app. Cheap, and it makes
    // the screen close the instant the user comes back rather than on the next
    // retry tick.
    const onVisible = () => { if (document.visibilityState === 'visible') void refresh(); };
    document.addEventListener('visibilitychange', onVisible);

    return () => {
      offRadio();
      document.removeEventListener('visibilitychange', onVisible);
      window.clearTimeout(deniedTimer.current);
    };
  }, [refresh]);

  // The radio came up: nothing is pending and nothing was refused.
  useEffect(() => {
    if (state !== 'on') return;
    window.clearTimeout(deniedTimer.current);
    setRequesting(false);
    setDenied(false);
  }, [state]);

  const requestEnable = useCallback(async () => {
    setDenied(false);
    setRequesting(true);
    window.clearTimeout(deniedTimer.current);

    try {
      await api.enableBluetooth();
    } catch {
      setRequesting(false);
      return;
    }

    // "Denied" IS INFERRED HERE, NOT REPORTED.
    //
    // The call returns the moment the prompt is raised — the user may still be
    // looking at it. Android gives the page no callback, and the honest signal
    // is the one the core already provides: if the radio has not come up a few
    // seconds after asking, something declined or nothing happened. The window
    // is comfortably longer than the core's 3 s transport retry, so a normal
    // "yes" always wins the race and this never fires on success.
    deniedTimer.current = window.setTimeout(() => {
      setRequesting(false);
      void refresh().then((current) => { if (current !== 'on') setDenied(true); });
    }, 8000);
  }, [refresh]);

  const openSettings = useCallback(async () => {
    try {
      await api.openBluetoothSettings();
    } catch {
      /* nothing useful to say: the settings screen simply did not open */
    }
  }, []);

  return {
    state,
    requesting,
    denied,
    requestEnable,
    openSettings,
    canPromptInApp,
    /**
     * `unsupported` counts as ready on purpose: that is desktop and browser
     * development, where there is no radio and the app must stay usable.
     */
    isReady: state === 'on' || state === 'unsupported',
  };
}
