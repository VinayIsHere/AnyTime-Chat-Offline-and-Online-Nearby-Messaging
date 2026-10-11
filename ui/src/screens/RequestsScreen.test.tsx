import { screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';

import { ApiError, type ChatRequest } from '../api/client';
import { createEventBus, renderAt } from '../test/harness';

const bus = vi.hoisted(() => ({ current: null as null | ReturnType<typeof createEventBus> }));

vi.mock('../api/events', () => ({
  onEvent: (...args: Parameters<ReturnType<typeof createEventBus>['onEvent']>) =>
    bus.current!.onEvent(...args),
}));

vi.mock('../api/client', async (importOriginal) => {
  const real = await importOriginal<typeof import('../api/client')>();
  return {
    ...real,
    api: {
      getRequests: vi.fn(),
      acceptRequest: vi.fn(),
      declineRequest: vi.fn(),
      reportPeer: vi.fn(),
    },
  };
});

import { api } from '../api/client';
import RequestsScreen from './RequestsScreen';

const mocked = vi.mocked(api);

const REQUEST: ChatRequest = {
  id: 'req1', peerId: '8899aabbccddeeff', nickname: 'mallory',
  handle: '@mallory·eeff', greeting: 'hey', at: '1m',
};

/** The request card, found by its handle so assertions stay inside it. */
async function card() {
  const name = await screen.findByText('@mallory·eeff');
  return name.closest('div[style]')!.parentElement as HTMLElement;
}

beforeEach(() => {
  bus.current = createEventBus();
  mocked.getRequests.mockResolvedValue([REQUEST]);
});

describe('RequestsScreen — declining and blocking', () => {
  it('declines with block, and the request leaves the list', async () => {
    mocked.declineRequest.mockResolvedValue(undefined);
    renderAt(<RequestsScreen />, '/requests');

    await userEvent.click(within(await card()).getByRole('button', { name: 'Decline and block' }));

    expect(mocked.declineRequest).toHaveBeenCalledWith('req1', true);
    await waitFor(() => expect(screen.queryByText('@mallory·eeff')).not.toBeInTheDocument());
    expect(screen.getByText('No requests yet')).toBeInTheDocument();
  });

  it('a plain decline does not block', async () => {
    mocked.declineRequest.mockResolvedValue(undefined);
    renderAt(<RequestsScreen />, '/requests');

    await userEvent.click(within(await card()).getByRole('button', { name: 'Decline' }));

    expect(mocked.declineRequest).toHaveBeenCalledWith('req1', false);
  });

  it('keeps the request, with working buttons, when the decline fails', async () => {
    mocked.declineRequest.mockRejectedValue(new ApiError(500, 'internal', 'boom'));
    renderAt(<RequestsScreen />, '/requests');

    const block = within(await card()).getByRole('button', { name: 'Decline and block' });
    await userEvent.click(block);

    await waitFor(() => expect(block).toBeEnabled());
    expect(screen.getByText('@mallory·eeff')).toBeInTheDocument();
  });

  it('shows a request that arrives over SSE, with the block option', async () => {
    mocked.getRequests.mockResolvedValue([]);
    renderAt(<RequestsScreen />, '/requests');
    expect(await screen.findByText('No requests yet')).toBeInTheDocument();

    bus.current!.emit('request.received', REQUEST);

    expect(await screen.findByRole('button', { name: 'Decline and block' })).toBeInTheDocument();
  });
});

describe('RequestsScreen — reporting a request', () => {
  it('reports the sender, and their request leaves the list', async () => {
    mocked.reportPeer.mockResolvedValue({ emailOpened: true, email: 'contact@techyskitter.com' });
    renderAt(<RequestsScreen />, '/requests');

    await userEvent.click(within(await card()).getByRole('button', { name: 'Report @mallory·eeff' }));
    await userEvent.click(screen.getByRole('radio', { name: 'Child safety concern' }));
    await userEvent.click(screen.getByRole('button', { name: 'Report and block' }));

    expect(mocked.reportPeer).toHaveBeenCalledWith('8899aabbccddeeff', 'child_safety');
    // Reporting blocks, and a blocked person's request is gone — no separate
    // decline call is made for it.
    expect(mocked.declineRequest).not.toHaveBeenCalled();
    expect(await screen.findByText('@mallory·eeff is blocked')).toBeInTheDocument();
    expect(screen.getByText('No requests yet')).toBeInTheDocument();

    await userEvent.click(screen.getByRole('button', { name: 'Done' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });
});
