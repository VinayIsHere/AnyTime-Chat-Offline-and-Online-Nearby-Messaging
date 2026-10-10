import { act, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import type { Account } from './api/client';
import { SPLASH_MIN_SHOW_MS } from './screens/SplashScreen';

vi.mock('./api/client', async (importOriginal) => {
  const real = await importOriginal<typeof import('./api/client')>();
  return { ...real, api: { getAccount: vi.fn() } };
});

// The radio never answers, so RequireBluetooth renders nothing: what is on
// screen is then either the splash or an empty page, with no screen fetching
// anything that would need standing in for.
vi.mock('./hooks/useBluetooth', () => ({
  useBluetooth: () => ({ state: 'unknown', isReady: false }),
}));

import { api } from './api/client';
import App from './App';

const mocked = vi.mocked(api);
const TAGLINE = 'Chat with people around you';

/** Lets the mocked account probe resolve and React act on it. */
const settle = () => act(async () => { await Promise.resolve(); });

beforeEach(() => {
  vi.useFakeTimers();
  mocked.getAccount.mockResolvedValue({ username: 'vinay' } as Account);
});

afterEach(() => {
  vi.useRealTimers();
  window.history.replaceState({}, '', '/');
});

describe('App — the launch splash', () => {
  it('stays up for its minimum time even when the core answers at once', async () => {
    window.history.replaceState({}, '', '/');
    render(<App />);
    await settle();

    // The account is already known; the splash is still being held.
    expect(screen.getByText(TAGLINE)).toBeInTheDocument();

    act(() => { vi.advanceTimersByTime(SPLASH_MIN_SHOW_MS - 1); });
    expect(screen.getByText(TAGLINE)).toBeInTheDocument();

    act(() => { vi.advanceTimersByTime(1); });
    expect(screen.queryByText(TAGLINE)).not.toBeInTheDocument();
  });

  it('does not hold someone who opened a chat from a notification', async () => {
    window.history.replaceState({}, '', '/chats/chat_1');
    render(<App />);

    // Only for as long as the account probe takes.
    expect(screen.getByText(TAGLINE)).toBeInTheDocument();
    await settle();
    expect(screen.queryByText(TAGLINE)).not.toBeInTheDocument();
  });

  it('keeps the splash up while the core has not answered, however long that takes', async () => {
    mocked.getAccount.mockReturnValue(new Promise(() => {}));
    window.history.replaceState({}, '', '/');
    render(<App />);
    await settle();

    act(() => { vi.advanceTimersByTime(SPLASH_MIN_SHOW_MS * 3); });
    expect(screen.getByText(TAGLINE)).toBeInTheDocument();
  });
});
