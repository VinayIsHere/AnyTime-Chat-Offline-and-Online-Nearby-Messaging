/**
 * The push channel: Server-Sent Events on GET /{token}/api/events.
 *
 * This REPLACES the 2.5 s polling loop the original NearbyScreen draft used.
 * The core emits domain events and they arrive here — the UI never polls.
 *
 * SSE rather than WebSocket because the channel is strictly one-way: the UI
 * already has HTTP for everything it sends. No upgrade handshake, no framing,
 * and EventSource reconnects on its own.
 *
 * Event names are defined in core/domain/Enums.h (EventType) and are part of
 * the local API contract — do not rename one side only.
 */

import { apiUrl } from './base';
import type { ChatSummary, DeliveryStatus, Message, NearbyPeer, ChatRequest } from './client';

export interface EventMap {
  'peer.discovered': NearbyPeer;
  'peer.lost': { peerId: string };
  'request.received': ChatRequest;
  'request.accepted': { request: ChatRequest; chat: ChatSummary };
  /** `peerId` travels with the id so the Nearby screen knows which peer to re-offer. */
  'request.declined': { id: string; peerId: string };
  'message.received': Message;
  'message.status': { id: string; status: DeliveryStatus };
  'radio.state': { available: boolean; detail: string };
}

export type EventName = keyof EventMap;

type Handler<E extends EventName> = (detail: EventMap[E]) => void;

const listeners = new Map<EventName, Set<Handler<EventName>>>();

let source: EventSource | null = null;

function ensureConnected(): void {
  if (source) return;

  source = new EventSource(apiUrl('events'));

  (Object.keys({} as EventMap) as EventName[]).forEach(() => {
    /* names are registered lazily below */
  });

  // Register a native listener per event name the moment someone subscribes.
  listeners.forEach((_set, name) => attach(name));

  source.onerror = () => {
    // EventSource retries on its own. Nothing to do but let it — surfacing a
    // reconnect as an error would make the UI flicker every time the core
    // restarts during development.
  };
}

function attach(name: EventName): void {
  if (!source) return;
  source.addEventListener(name, (ev) => {
    const set = listeners.get(name);
    if (!set || set.size === 0) return;

    let detail: unknown = null;
    try {
      detail = JSON.parse((ev as MessageEvent).data);
    } catch {
      return; // malformed frame — drop it rather than throwing into the UI
    }
    set.forEach((fn) => fn(detail as never));
  });
}

/**
 * Subscribes to one event. Returns an unsubscribe function suitable for a
 * useEffect cleanup.
 */
export function onEvent<E extends EventName>(name: E, handler: Handler<E>): () => void {
  let set = listeners.get(name);
  const isFirstForName = !set;

  if (!set) {
    set = new Set();
    listeners.set(name, set);
  }
  set.add(handler as Handler<EventName>);

  ensureConnected();
  if (isFirstForName) attach(name);

  return () => {
    set!.delete(handler as Handler<EventName>);
  };
}

/** Closes the stream. Called on sign-out and panic wipe. */
export function closeEvents(): void {
  source?.close();
  source = null;
  listeners.clear();
}
