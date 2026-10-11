import { fireEvent, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { ApiError, type ChatSummary, type MediaRef, type Message } from '../api/client';
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
      reportPeer: vi.fn(),
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

  it('resends a stored failed message by id and shows it sending again', async () => {
    mocked.retryMessage.mockResolvedValue(undefined);
    openChat(CHAT, [message({ id: 'bad', status: 'failed' })]);

    await userEvent.click(await screen.findByRole('button', { name: 'Retry sending' }));

    expect(mocked.retryMessage).toHaveBeenCalledWith('bad');
    expect(mocked.sendMessage).not.toHaveBeenCalled();
    expect(await screen.findByText(/Sending/)).toBeInTheDocument();
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
    expect(await screen.findByText(/Sending/)).toBeInTheDocument();
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

describe('ChatScreen — sending a photo', () => {
  const photo = new File(['raw-camera-bytes'], 'photo.jpg', { type: 'image/jpeg' });
  const MEDIA: MediaRef = { id: 'media_1', mimeType: 'image/jpeg', bytes: 5 };

  let getContext: { mockRestore(): void };
  let toBlob: { mockRestore(): void };

  // jsdom can neither decode an image nor draw one, so the three browser calls
  // shrinkForSending makes are stood in for. What is under test is the thread.
  beforeEach(() => {
    mocked.sendMedia.mockReset();
    mocked.retryMessage.mockReset();
    vi.stubGlobal('createImageBitmap', vi.fn(async () => ({ width: 400, height: 300, close: () => {} })));
    getContext = vi.spyOn(HTMLCanvasElement.prototype, 'getContext')
      .mockReturnValue({ drawImage: () => {} } as never);
    toBlob = vi.spyOn(HTMLCanvasElement.prototype, 'toBlob')
      .mockImplementation((done: BlobCallback) => done(new Blob(['small'], { type: 'image/jpeg' })));
  });

  afterEach(() => {
    vi.unstubAllGlobals();
    getContext.mockRestore();
    toBlob.mockRestore();
  });

  async function attach(container: HTMLElement) {
    await screen.findByPlaceholderText('Message');
    await userEvent.upload(container.querySelector<HTMLInputElement>('input[type="file"]')!, photo);
  }

  it('shows the photo at once, as sending, before the upload has finished', async () => {
    let finish!: (m: Message) => void;
    mocked.sendMedia.mockReturnValue(new Promise<Message>((resolve) => { finish = resolve; }));
    const { container } = openChat();

    await attach(container);

    // In the thread while the core is still busy with it.
    const preview = await screen.findByAltText('Photo');
    expect(preview).toHaveAttribute('src', 'blob:test');
    expect(screen.getByText(/Sending/)).toBeInTheDocument();
    expect(screen.queryByText(/Delivered/)).not.toBeInTheDocument();

    // What goes to the core is the compressed copy, never the camera original.
    await waitFor(() => expect(mocked.sendMedia).toHaveBeenCalledTimes(1));
    const [, sent, , clientMessageId] = mocked.sendMedia.mock.calls[0];
    expect(sent).not.toBe(photo);
    expect(sent.size).toBe(5);

    finish(message({ id: 'stored', clientMessageId, body: '', status: 'sent', media: MEDIA }));

    // Same bubble, same picture — only the receipt moves on.
    expect(await screen.findByText(/Sent/)).toBeInTheDocument();
    expect(screen.getByAltText('Photo')).toHaveAttribute('src', 'blob:test');

    bus.current!.emit('message.status', { id: 'stored', status: 'delivered' });
    expect(await screen.findByText(/Delivered/)).toBeInTheDocument();
  });

  it('keeps a photo whose upload failed, and retry resends the same bytes', async () => {
    mocked.sendMedia
      .mockRejectedValueOnce(new ApiError(0, 'unreachable', 'down'))
      .mockImplementationOnce(async (_chat, _image, caption, clientMessageId) =>
        message({ id: 'stored', clientMessageId, body: caption, status: 'queued', media: MEDIA }));
    const { container } = openChat();

    await attach(container);

    const retry = await screen.findByRole('button', { name: 'Retry sending' });
    expect(retry).toHaveTextContent('Not sent · Tap to retry');
    expect(screen.getByAltText('Photo')).toBeInTheDocument();

    await userEvent.click(retry);

    await waitFor(() => expect(mocked.sendMedia).toHaveBeenCalledTimes(2));
    const [first, second] = mocked.sendMedia.mock.calls;
    expect(second[1]).toBe(first[1]);   // the same compressed photo
    expect(second[3]).toBe(first[3]);   // under the same client id
    expect(mocked.retryMessage).not.toHaveBeenCalled();
    expect(await screen.findByText(/Sending/)).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Retry sending' })).not.toBeInTheDocument();
    expect(screen.getByAltText('Photo')).toBeInTheDocument();
  });

  it('takes the bubble back down when the file cannot be read as an image', async () => {
    vi.stubGlobal('createImageBitmap', vi.fn(async () => { throw new Error('not an image'); }));
    const { container } = openChat();

    await attach(container);

    expect(await screen.findByText('Could not read that image.')).toBeInTheDocument();
    expect(screen.queryByAltText('Photo')).not.toBeInTheDocument();
    expect(mocked.sendMedia).not.toHaveBeenCalled();
  });

  it('follows the bottom of the thread again once a photo has loaded', async () => {
    const scrollTo = vi.spyOn(Element.prototype, 'scrollTo');
    openChat(CHAT, [
      message({ id: 'p', clientMessageId: 'x', mine: false, body: '', status: 'delivered', media: MEDIA }),
    ]);

    const img = await screen.findByAltText('Photo');
    scrollTo.mockClear();
    fireEvent.load(img);

    expect(scrollTo).toHaveBeenCalledWith({ top: 1e6 });
    scrollTo.mockRestore();
  });
});

describe('ChatScreen — the receipt on a message that has not gone yet', () => {
  it('says sending while the peer is in range', async () => {
    openChat(CHAT, [message({ status: 'queued' })]);
    expect(await screen.findByText(/Sending/)).toBeInTheDocument();
    expect(screen.getByLabelText('Not sent yet')).toBeInTheDocument();
  });

  it('says queued, not sending, while the peer is out of range', async () => {
    openChat({ ...CHAT, reachable: false }, [message({ status: 'queued' })]);
    expect(await screen.findByText(/Queued/)).toBeInTheDocument();
    expect(screen.queryByText(/Sending/)).not.toBeInTheDocument();
  });
});

describe('ChatScreen — reporting a contact', () => {
  it('reports from the chat menu, and the chat ends up blocked', async () => {
    mocked.reportPeer.mockResolvedValue({ emailOpened: true, email: 'contact@techyskitter.com' });
    openChat();

    await userEvent.click(await screen.findByRole('button', { name: 'Chat options' }));
    await userEvent.click(screen.getByRole('menuitem', { name: /Report @alice·6677/ }));

    // The menu gives way to the sheet rather than sitting behind it.
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    const sheet = screen.getByRole('dialog', { name: 'Report @alice·6677' });
    expect(sheet).toBeInTheDocument();

    await userEvent.click(screen.getByRole('radio', { name: 'Harassment or threats' }));
    await userEvent.click(screen.getByRole('button', { name: 'Report and block' }));

    expect(mocked.reportPeer).toHaveBeenCalledWith('0011223344556677', 'harassment');
    expect(await screen.findByText('@alice·6677 is blocked')).toBeInTheDocument();
    // Behind the sheet the composer has already given way to the blocked bar.
    expect(screen.queryByPlaceholderText('Message')).not.toBeInTheDocument();

    await userEvent.click(screen.getByRole('button', { name: 'Done' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(screen.getByText(/You blocked @alice·6677/)).toBeInTheDocument();
  });

  it('can report someone who is already blocked', async () => {
    openChat({ ...CHAT, blocked: true });

    await userEvent.click(await screen.findByRole('button', { name: 'Chat options' }));
    expect(screen.getByRole('menuitem', { name: /Report @alice·6677/ })).toBeInTheDocument();
  });
});
