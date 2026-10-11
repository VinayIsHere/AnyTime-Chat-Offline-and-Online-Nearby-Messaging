import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';

import { ApiError } from '../api/client';

vi.mock('../api/client', async (importOriginal) => {
  const real = await importOriginal<typeof import('../api/client')>();
  return { ...real, api: { reportPeer: vi.fn() } };
});

import { api } from '../api/client';
import ReportSheet from './ReportSheet';

const mocked = vi.mocked(api);
const EMAIL = 'contact@techyskitter.com';

function open() {
  const onClose = vi.fn();
  const onReported = vi.fn();
  render(
    <ReportSheet peerId="8899aabbccddeeff" handle="@mallory·eeff" onClose={onClose} onReported={onReported} />,
  );
  return { onClose, onReported };
}

beforeEach(() => {
  mocked.reportPeer.mockReset();
});

describe('ReportSheet', () => {
  it('will not report until a reason is chosen', async () => {
    open();

    expect(screen.getByRole('button', { name: 'Report and block' })).toBeDisabled();

    await userEvent.click(screen.getByRole('radio', { name: 'Spam or scam' }));
    expect(screen.getByRole('button', { name: 'Report and block' })).toBeEnabled();
    expect(mocked.reportPeer).not.toHaveBeenCalled();
  });

  it('says up front that the block is immediate and the email goes when online', () => {
    open();
    expect(screen.getByText(/blocked straight away, even with no internet/)).toBeInTheDocument();
    expect(screen.getByText(/sends it when you are\s+online/)).toBeInTheDocument();
  });

  it('reports with the chosen reason, then explains the email and shows the address', async () => {
    mocked.reportPeer.mockResolvedValue({ emailOpened: true, email: EMAIL });
    const { onClose, onReported } = open();

    await userEvent.click(screen.getByRole('radio', { name: 'Child safety concern' }));
    await userEvent.click(screen.getByRole('button', { name: 'Report and block' }));

    expect(mocked.reportPeer).toHaveBeenCalledWith('8899aabbccddeeff', 'child_safety');
    expect(await screen.findByText('@mallory·eeff is blocked')).toBeInTheDocument();
    // The parent hears at once — it must not wait for the sheet to be dismissed.
    expect(onReported).toHaveBeenCalledTimes(1);
    expect(onClose).not.toHaveBeenCalled();

    expect(screen.getByText(/Press Send there to finish it/)).toBeInTheDocument();
    expect(screen.getByText(/send it once you are back online/)).toBeInTheDocument();
    expect(screen.getByText(EMAIL)).toBeInTheDocument();

    await userEvent.click(screen.getByRole('button', { name: 'Done' }));
    expect(onClose).toHaveBeenCalledTimes(1);
  });

  it('gives the address to write to when the phone has no mail app', async () => {
    mocked.reportPeer.mockResolvedValue({ emailOpened: false, email: EMAIL });
    open();

    await userEvent.click(screen.getByRole('radio', { name: 'Harassment or threats' }));
    await userEvent.click(screen.getByRole('button', { name: 'Report and block' }));

    // Still blocked — only the email half did not happen.
    expect(await screen.findByText('@mallory·eeff is blocked')).toBeInTheDocument();
    expect(screen.getByText(/No mail app opened on this phone/)).toBeInTheDocument();
    expect(screen.getByText(EMAIL)).toBeInTheDocument();
  });

  it('stays open and says so when the report could not be made', async () => {
    mocked.reportPeer.mockRejectedValue(new ApiError(500, 'internal', 'boom'));
    const { onReported } = open();

    await userEvent.click(screen.getByRole('radio', { name: 'Something else' }));
    await userEvent.click(screen.getByRole('button', { name: 'Report and block' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('Could not report just now');
    expect(onReported).not.toHaveBeenCalled();
    // Nothing was claimed: it does not say they are blocked.
    expect(screen.queryByText(/is blocked/)).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Report and block' })).toBeEnabled();
  });

  it('closes without reporting on Cancel', async () => {
    const { onClose } = open();
    await userEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(onClose).toHaveBeenCalledTimes(1);
    expect(mocked.reportPeer).not.toHaveBeenCalled();
  });
});
