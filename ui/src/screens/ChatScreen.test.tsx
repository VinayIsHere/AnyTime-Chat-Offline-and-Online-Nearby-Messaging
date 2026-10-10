import { screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';

import { ApiError, type ChatSummary, type Message } from '../api/client';
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
      getChat: vi.fn(),
      getMessages: vi.fn(),
      sendMessage: vi.fn(),
      retryMessage: vi.fn(),
      blockPeer: vi.fn(),
      unblockPeer: vi.fn(),
      sendMedia: vi.fn(),
      mediaUrl: (id: string) => '/media/' + id,
    },
  };
});

import { api } from '../api/client';
import ChatScreen from './ChatScreen';

const mocked = vi.mocked(api);

const CHAT: ChatSummary = {
  id: 'chat1', peerId: '0011223344556677', handle: '@alice·6677',
  lastMessage: '', at: 'now', unread: 0, reachable: true, blocked: false,
};

function message(over: Partial<Message>): Message {
  return {
    id: 'm1', chatId: 'chat1', clientMessageId: 'c1', body: 'hello',
    mine: true, at: '2m', status: 'sent', ...over,
  };
}

function openChat(chat: ChatSummary = CHAT, messages: Message[] = []) {
  mocked.getChat.mockResolvedValue(chat);
  // The API pages newest first; the screen reverses into reading order.
  mocked.getMessages.mockResolvedValue({ messages: [...messages].reverse(), nextCursor: null, hasMore: false });
  return renderAt(<ChatScreen />, '/chats/chat1', '/chats/:chatId');
}

beforeEach(() => {
  bus.current = createEventBus();
});

describe('ChatScreen — resending a failed message', () => {
  it('offers retry only on a failed message', async () => {
    openChat(CHAT, [
      message({ id: 'ok', clientMessageId: 'a', body: 'arrived', status: 'delivered' }),
      message({ id: 'bad', clientMessageId: 'b', body: 'lost', status: 'failed' }),
    ]);

    expect(await screen.findByText('lost')).toBeInTheDocument();
    const retries = screen.getAllByRole('button', { name: 'Retry sending' });
    expect(retries).toHaveLength(1);
    expect(retries[0]).toHaveTextContent('Not sent · Tap to retry');
  });

  it('resends a stored failed message by id and shows it queued again', async () => {
    mocked.retryMessage.mockResolvedValue(undefined);
    openChat(CHAT, [message({ id: 'bad', status: 'failed' })]);

    await userEvent.click(await screen.findByRole('button', { name: 'Retry sending' }));

    expect(mocked.retryMessage).toHaveBeenCalledWith('bad');
    expect(mocked.sendMessage).not.toHaveBeenCalled();
    expect(await screen.findByText(/Queued/)).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Retry sending' })).not.toBeInTheDocument();
  });

  it('follows the core over SSE after a resend', async () => {
    mocked.retryMessage.mockResolvedValue(undefined);
    openChat(CHAT, [message({ id: 'bad', status: 'failed' })]);
    await userEvent.click(await screen.findByRole('button', { name: 'Retry sending' }));

    bus.current!.emit('message.status', { id: 'bad', status: 'delivered' });
    expect(await screen.findByText(/Delivered/)).toBeInTheDocument();
  });

  it('re-POSTs a message the core never stored, reusing its client id', async () => {
    // First attempt never reaches the core; the second does.
    mocked.sendMessage
      .mockRejectedValueOnce(new ApiError(0, 'unreachable', 'down'))
      .mockImplementationOnce(async (_chat, body, clientMessageId) =>
        message({ id: 'stored', clientMessageId, body, status: 'queued' }));
    openChat();

    const input = await screen.findByPlaceholderText('Message');
    await userEvent.type(input, 'are you there{Enter}');

    const retry = await screen.findByRole('button', { name: 'Retry sending' });
    const firstClientId = mocked.sendMessage.mock.calls[0][2];

    await userEvent.click(retry);

    await waitFor(() => expect(mocked.sendMessage).toHaveBeenCalledTimes(2));
    expect(mocked.sendMessage.mock.calls[1]).toEqual(['chat1', 'are you there', firstClientId]);
    expect(mocked.retryMessage).not.toHaveBeenCalled();
    expect(await screen.findByText(/Queued/)).toBeInTheDocument();
  });

  it('puts the message back to failed and says why when the resend is refused', async () => {
    mocked.retryMessage.mockRejectedValue(new ApiError(403, 'permission_denied', 'blocked'));
    openChat(CHAT, [message({ id: 'bad', status: 'failed' })]);

    await userEvent.click(await screen.findByRole('button', { name: 'Retry sending' }));

    expect(await screen.findByText('You blocked this person, so messages cannot be sent.')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Retry sending' })).toBeInTheDocument();
  });
});

describe('ChatScreen — blocking a contact', () => {
  it('blocks from the chat menu and replaces the composer', async () => {
    mocked.blockPeer.mockResolvedValue(undefined);
    openChat();

    await userEvent.click(await screen.findByRole('button', { name: 'Chat options' }));
    await userEvent.click(screen.getByRole('menuitem', { name: /Block @alice·6677/ }));

    expect(mocked.blockPeer).toHaveBeenCalledWith('0011223344556677');
    expect(await screen.findByText(/You blocked @alice·6677/)).toBeInTheDocument();
    expect(screen.queryByPlaceholderText('Message')).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Send' })).not.toBeInTheDocument();
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
  });

  it('closes the menu when tapping outside it, without blocking', async () => {
    openChat();
    await userEvent.click(await screen.findByRole('button', { name: 'Chat options' }));
    expect(screen.getByRole('menu')).toBeInTheDocument();

    await userEvent.click(screen.getByRole('button', { name: 'Chat options' }));
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(mocked.blockPeer).not.toHaveBeenCalled();
  });

  it('opens an already-blocked chat with an unblock prompt, and unblocking restores the composer', async () => {
    mocked.unblockPeer.mockResolvedValue(undefined);
    openChat({ ...CHAT, blocked: true });

    expect(await screen.findByText(/You blocked @alice·6677/)).toBeInTheDocument();
    expect(screen.queryByPlaceholderText('Message')).not.toBeInTheDocument();

    await userEvent.click(screen.getByRole('button', { name: 'Unblock' }));

    expect(mocked.unblockPeer).toHaveBeenCalledWith('0011223344556677');
    expect(await screen.findByPlaceholderText('Message')).toBeInTheDocument();
  });

  it('offers unblock from the menu of a blocked chat', async () => {
    mocked.unblockPeer.mockResolvedValue(undefined);
    openChat({ ...CHAT, blocked: true });

    await userEvent.click(await screen.findByRole('button', { name: 'Chat options' }));
    await userEvent.click(screen.getByRole('menuitem', { name: /Unblock @alice·6677/ }));

    expect(mocked.unblockPeer).toHaveBeenCalledWith('0011223344556677');
    expect(mocked.blockPeer).not.toHaveBeenCalled();
  });

  it('keeps the composer and reports it when blocking fails', async () => {
    mocked.blockPeer.mockRejectedValue(new ApiError(500, 'internal', 'boom'));
    openChat();

    await userEvent.click(await screen.findByRole('button', { name: 'Chat options' }));
    await userEvent.click(screen.getByRole('menuitem', { name: /Block/ }));

    expect(await screen.findByText('Could not block this person.')).toBeInTheDocument();
    expect(screen.getByPlaceholderText('Message')).toBeInTheDocument();
  });
});
