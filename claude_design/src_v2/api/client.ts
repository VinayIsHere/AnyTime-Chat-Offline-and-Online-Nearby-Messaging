const BASE_URL = import.meta.env?.VITE_API_URL ?? 'http://localhost:8080/api';

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(BASE_URL + path, {
    headers: { 'Content-Type': 'application/json' },
    credentials: 'include',
    ...init,
  });
  if (!res.ok) throw new Error((await res.text()) || res.statusText);
  return res.json() as Promise<T>;
}

export type Gender = 'Male' | 'Female' | 'Other' | 'Prefer not to say';

export type Account = { userId: string; username: string; dob: string; gender: Gender };

export type NearbyUser = {
  id: string;
  username: string;
  distanceMeters: number;
  signal: 'very strong' | 'strong' | 'weak';
};
export type ChatSummary = {
  id: string;
  username: string;
  lastMessage: string;
  at: string;
  unread: number;
  online: boolean;
};
export type Message = {
  id: string;
  chatId: string;
  body: string;
  mine: boolean;
  at: string;
  status: 'queued' | 'sent' | 'delivered' | 'read';
};

export const api = {
  /** v1 has no phone/email verification — the account is created locally. */
  checkUsername: (username: string) => request<{ available: boolean }>('/account/username-available?u=' + encodeURIComponent(username)),
  createAccount: (p: { username: string; dob: string; gender: Gender }) => request<Account>('/account', { method: 'POST', body: JSON.stringify(p) }),
  getAccount: () => request<Account & { discoverable: boolean; acceptFrom: string }>('/account'),
  updateAccount: (p: Partial<{ username: string; gender: Gender; discoverable: boolean; acceptFrom: string }>) => request<{ ok: true }>('/account', { method: 'PATCH', body: JSON.stringify(p) }),

  /** Radio came back up: restart the mesh and flush anything queued while it was off. */
  bluetoothReady: () => request<{ queuedFlushed: number }>('/bluetooth/ready', { method: 'POST' }),
  bluetoothStatus: () => request<{ meshActive: boolean; queued: number }>('/bluetooth/status'),

  startScan: () => request<{ scanId: string }>('/nearby/scan', { method: 'POST' }),
  getNearby: (scanId: string) => request<NearbyUser[]>('/nearby/' + scanId),
  sayHi: (userId: string) => request<{ chatId: string }>('/chats', { method: 'POST', body: JSON.stringify({ userId }) }),

  getChats: () => request<ChatSummary[]>('/chats'),
  getMessages: (chatId: string) => request<Message[]>('/chats/' + chatId + '/messages'),
  sendMessage: (chatId: string, body: string) => request<Message>('/chats/' + chatId + '/messages', { method: 'POST', body: JSON.stringify({ body }) }),
};
