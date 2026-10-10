import { screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';

import { ApiError, type Account, type BlockedPeer } from '../api/client';
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
      getAccount: vi.fn(),
      updateAccount: vi.fn(),
      getBlocked: vi.fn(),
      unblockPeer: vi.fn(),
      getRequests: vi.fn(), // BottomNav's badge
    },
  };
});

import { api } from '../api/client';
import ProfileScreen from './ProfileScreen';

const mocked = vi.mocked(api);

const ACCOUNT: Account = {
  peerId: '0123456789abcdef', username: 'priya', handle: '@priya·cdef',
  dob: '2000-01-01', gender: 'Female', discoverable: true, acceptFrom: 'everyone',
};

const BLOCKED: BlockedPeer[] = [
  { peerId: '8899aabbccddeeff', nickname: 'mallory', handle: '@mallory·eeff', at: '2d' },
  { peerId: 'fedcba9876543210', nickname: '', handle: '@3210', at: 'now' },
];

function open() {
  return renderAt(<ProfileScreen />, '/profile');
}

beforeEach(() => {
  bus.current = createEventBus();
  mocked.getAccount.mockResolvedValue(ACCOUNT);
  mocked.getBlocked.mockResolvedValue(BLOCKED);
  mocked.getRequests.mockResolvedValue([]);
  // Echo the change back as the core does: the stored account, updated.
  mocked.updateAccount.mockImplementation(async (change) => ({ ...ACCOUNT, ...change }));
});

describe('ProfileScreen — first render', () => {
  it('shows no made-up profile while the account loads', async () => {
    let resolve!: (a: Account) => void;
    mocked.getAccount.mockReturnValue(new Promise<Account>((r) => { resolve = r; }));
    open();

    expect(screen.getByLabelText('Loading profile')).toBeInTheDocument();
    // The old screen started from a hard-coded "vinay, 25, Male".
    expect(screen.queryByText(/vinay/i)).not.toBeInTheDocument();
    expect(screen.queryByText(/25 ·/)).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Edit profile' })).not.toBeInTheDocument();

    resolve(ACCOUNT);
    expect(await screen.findByText('@priya·cdef')).toBeInTheDocument();
    expect(screen.getByText(/Female · On this device/)).toBeInTheDocument();
  });

  it('offers a retry when the account cannot be loaded', async () => {
    mocked.getAccount.mockRejectedValueOnce(new ApiError(0, 'unreachable', 'down'));
    open();

    await userEvent.click(await screen.findByRole('button', { name: 'Try again' }));

    expect(await screen.findByText('@priya·cdef')).toBeInTheDocument();
    expect(mocked.getAccount).toHaveBeenCalledTimes(2);
  });
});

describe('ProfileScreen — changing the username', () => {
  it('saves a new name and shows the handle the core returns', async () => {
    mocked.updateAccount.mockResolvedValue({ ...ACCOUNT, username: 'priya_s', handle: '@priya_s·cdef' });
    open();

    await userEvent.click(await screen.findByRole('button', { name: 'Edit profile' }));
    const field = screen.getByLabelText('Username');
    expect(field).toHaveValue('priya');

    await userEvent.clear(field);
    await userEvent.type(field, 'priya_s');
    await userEvent.click(screen.getByRole('button', { name: 'Save' }));

    expect(mocked.updateAccount).toHaveBeenCalledWith({ username: 'priya_s' });
    expect(await screen.findByText('@priya_s·cdef')).toBeInTheDocument();
    expect(screen.queryByLabelText('Username')).not.toBeInTheDocument();
  });

  it('keeps the field to characters the core accepts', async () => {
    open();
    await userEvent.click(await screen.findByRole('button', { name: 'Edit profile' }));
    const field = screen.getByLabelText('Username');

    await userEvent.clear(field);
    await userEvent.type(field, 'Priya S!');

    expect(field).toHaveValue('priyas');
  });

  it('will not save a name that is too short', async () => {
    open();
    await userEvent.click(await screen.findByRole('button', { name: 'Edit profile' }));
    const field = screen.getByLabelText('Username');

    await userEvent.clear(field);
    await userEvent.type(field, 'pr');

    expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
    expect(screen.getByText(/3–20 characters/)).toBeInTheDocument();
    await userEvent.type(field, '{Enter}');
    expect(mocked.updateAccount).not.toHaveBeenCalled();
  });

  it('does not call the core when the name is unchanged', async () => {
    open();
    await userEvent.click(await screen.findByRole('button', { name: 'Edit profile' }));
    await userEvent.click(screen.getByRole('button', { name: 'Save' }));

    expect(mocked.updateAccount).not.toHaveBeenCalled();
    expect(screen.getByText('@priya·cdef')).toBeInTheDocument();
  });

  it('cancelling discards the draft', async () => {
    open();
    await userEvent.click(await screen.findByRole('button', { name: 'Edit profile' }));
    await userEvent.type(screen.getByLabelText('Username'), 'zzz');
    await userEvent.click(screen.getByRole('button', { name: 'Cancel' }));

    expect(screen.getByText('@priya·cdef')).toBeInTheDocument();
    expect(mocked.updateAccount).not.toHaveBeenCalled();
  });

  it('stays in the editor and explains when the core refuses the name', async () => {
    mocked.updateAccount.mockRejectedValue(new ApiError(400, 'invalid_argument', 'Invalid username.'));
    open();
    await userEvent.click(await screen.findByRole('button', { name: 'Edit profile' }));
    await userEvent.type(screen.getByLabelText('Username'), '_x');
    await userEvent.click(screen.getByRole('button', { name: 'Save' }));

    expect(await screen.findByRole('alert')).toHaveTextContent(/3–20 characters/);
    expect(screen.getByLabelText('Username')).toBeInTheDocument();
  });
});

describe('ProfileScreen — who can send requests', () => {
  it('opens the choice and switches to no one', async () => {
    open();
    const row = await screen.findByRole('button', { name: /Accept messages from/ });
    expect(row).toHaveTextContent('Everyone nearby');

    await userEvent.click(row);
    const group = screen.getByRole('radiogroup', { name: 'Accept messages from' });
    expect(within(group).getByRole('radio', { name: /Everyone nearby/ })).toHaveAttribute('aria-checked', 'true');

    await userEvent.click(within(group).getByRole('radio', { name: /No one/ }));

    expect(mocked.updateAccount).toHaveBeenCalledWith({ acceptFrom: 'nobody' });
    await waitFor(() => expect(row).toHaveTextContent('No one'));
    expect(within(group).getByRole('radio', { name: /No one/ })).toHaveAttribute('aria-checked', 'true');
  });

  it('does not re-save the option already chosen', async () => {
    open();
    await userEvent.click(await screen.findByRole('button', { name: /Accept messages from/ }));
    await userEvent.click(screen.getByRole('radio', { name: /Everyone nearby/ }));

    expect(mocked.updateAccount).not.toHaveBeenCalled();
  });

  it('rolls back and reports it when the change cannot be saved', async () => {
    mocked.updateAccount.mockRejectedValue(new ApiError(500, 'internal', 'boom'));
    open();
    const row = await screen.findByRole('button', { name: /Accept messages from/ });
    await userEvent.click(row);
    await userEvent.click(screen.getByRole('radio', { name: /No one/ }));

    expect(await screen.findByRole('alert')).toHaveTextContent('Could not save that change.');
    expect(row).toHaveTextContent('Everyone nearby');
  });

  it('the discoverable switch rolls back too', async () => {
    mocked.updateAccount.mockRejectedValue(new ApiError(500, 'internal', 'boom'));
    open();
    const toggle = await screen.findByRole('button', { name: 'Discoverable nearby' });
    await userEvent.click(toggle);

    expect(mocked.updateAccount).toHaveBeenCalledWith({ discoverable: false });
    await waitFor(() => expect(toggle).toHaveAttribute('aria-pressed', 'true'));
  });
});

describe('ProfileScreen — blocked users', () => {
  it('counts, lists and unblocks', async () => {
    mocked.unblockPeer.mockResolvedValue(undefined);
    open();

    const row = await screen.findByRole('button', { name: /Blocked users/ });
    await waitFor(() => expect(row).toHaveTextContent('2 people'));

    await userEvent.click(row);
    expect(screen.getByText('@mallory·eeff')).toBeInTheDocument();
    expect(screen.getByText('Blocked 2d ago')).toBeInTheDocument();
    // Never named: the fingerprint handle still tells rows apart.
    expect(screen.getByText('@3210')).toBeInTheDocument();
    expect(screen.getByText('Blocked just now')).toBeInTheDocument();

    await userEvent.click(screen.getByRole('button', { name: 'Unblock @mallory·eeff' }));

    expect(mocked.unblockPeer).toHaveBeenCalledWith('8899aabbccddeeff');
    await waitFor(() => expect(screen.queryByText('@mallory·eeff')).not.toBeInTheDocument());
    expect(row).toHaveTextContent('1 person');
  });

  it('says so when nobody is blocked', async () => {
    mocked.getBlocked.mockResolvedValue([]);
    open();

    const row = await screen.findByRole('button', { name: /Blocked users/ });
    await waitFor(() => expect(row).toHaveTextContent('None'));
    await userEvent.click(row);
    expect(screen.getByText(/Nobody\./)).toBeInTheDocument();
  });

  it('keeps the row and reports it when unblocking fails', async () => {
    mocked.unblockPeer.mockRejectedValue(new ApiError(500, 'internal', 'boom'));
    open();
    await userEvent.click(await screen.findByRole('button', { name: /Blocked users/ }));
    await userEvent.click(await screen.findByRole('button', { name: 'Unblock @mallory·eeff' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('Could not unblock @mallory·eeff.');
    expect(screen.getByText('@mallory·eeff')).toBeInTheDocument();
  });

  it('does not claim "None" when the list could not be loaded', async () => {
    mocked.getBlocked.mockRejectedValue(new ApiError(0, 'unreachable', 'down'));
    open();

    const row = await screen.findByRole('button', { name: /Blocked users/ });
    expect(row).not.toHaveTextContent('None');
    await userEvent.click(row);
    expect(screen.getByText('Could not load the block list.')).toBeInTheDocument();
  });
});
