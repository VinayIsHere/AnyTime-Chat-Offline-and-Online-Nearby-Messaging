/**
 * Shared test plumbing: a router to render screens inside, and a stand-in for
 * the SSE channel that a test can push events through.
 */
import type { ReactElement } from 'react';
import { render } from '@testing-library/react';
import { MemoryRouter, Route, Routes } from 'react-router-dom';

import type { EventMap, EventName } from '../api/events';

type Handler = (detail: never) => void;

/**
 * Records every `onEvent` subscription so a test can play the core's part:
 * `emit('message.status', …)` reaches exactly the handlers a screen registered.
 */
export function createEventBus() {
  const handlers = new Map<EventName, Set<Handler>>();

  function onEvent<E extends EventName>(name: E, handler: (detail: EventMap[E]) => void) {
    const set = handlers.get(name) ?? new Set<Handler>();
    set.add(handler as Handler);
    handlers.set(name, set);
    return () => set.delete(handler as Handler);
  }

  function emit<E extends EventName>(name: E, detail: EventMap[E]) {
    handlers.get(name)?.forEach((h) => (h as (d: EventMap[E]) => void)(detail));
  }

  return { onEvent, emit };
}

/** Renders one screen at `path`, matched against `pattern` so params resolve. */
export function renderAt(ui: ReactElement, path: string, pattern = path) {
  return render(
    <MemoryRouter initialEntries={[path]}>
      <Routes>
        <Route path={pattern} element={ui} />
        <Route path="*" element={<div>elsewhere</div>} />
      </Routes>
    </MemoryRouter>,
  );
}
