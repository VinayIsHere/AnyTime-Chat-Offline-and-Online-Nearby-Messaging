import { useCallback, useEffect, useState } from 'react';
import { api } from '../api/client';

export type BluetoothState = 'unknown' | 'off' | 'unauthorized' | 'on';

/**
 * Single source of truth for radio state.
 *
 * The native shell (Capacitor / React Native bridge) is expected to expose
 * window.AnyTimeBluetooth. On Android it can flip the radio with the system
 * ACTION_REQUEST_ENABLE dialog, which is why `canPromptInApp` exists — on iOS
 * there is no such dialog and the UI must send the user to Settings instead.
 */
declare global {
  interface Window {
    AnyTimeBluetooth?: {
      getState(): Promise<BluetoothState>;
      /** Android: shows the system "Allow AnyTime to turn on Bluetooth?" dialog. */
      requestEnable(): Promise<boolean>;
      openSettings(): Promise<void>;
      onStateChange(cb: (s: BluetoothState) => void): () => void;
      canPromptInApp: boolean;
    };
  }
}

export function useBluetooth() {
  const bridge = typeof window === 'undefined' ? undefined : window.AnyTimeBluetooth;
  const [state, setState] = useState<BluetoothState>('unknown');
  const [requesting, setRequesting] = useState(false);
  const [denied, setDenied] = useState(false);

  useEffect(() => {
    if (!bridge) { setState('on'); return; } // browser dev: don't block the app
    let alive = true;
    bridge.getState().then((s) => { if (alive) setState(s); }).catch(() => {});
    const off = bridge.onStateChange((s) => setState(s));
    return () => { alive = false; off(); };
  }, [bridge]);

  /** Tell the backend the radio came up so it can restart the mesh + flush the queue. */
  useEffect(() => {
    if (state === 'on') api.bluetoothReady().catch(() => {});
  }, [state]);

  const requestEnable = useCallback(async () => {
    if (!bridge) return;
    setRequesting(true);
    setDenied(false);
    try {
      const ok = bridge.canPromptInApp ? await bridge.requestEnable() : (await bridge.openSettings(), false);
      if (bridge.canPromptInApp && !ok) setDenied(true);
      setState(await bridge.getState());
    } finally {
      setRequesting(false);
    }
  }, [bridge]);

  const openSettings = useCallback(() => bridge?.openSettings(), [bridge]);

  return {
    state,
    requesting,
    denied,
    requestEnable,
    openSettings,
    canPromptInApp: bridge?.canPromptInApp ?? false,
    isReady: state === 'on',
  };
}
