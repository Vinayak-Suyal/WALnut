// WALnut API client
// Wraps every backend call with typed returns and consistent error handling.

import type {
  PlaceBidRequest,
  PlaceBidResponse,
  CreateLotRequest,
  HostInfo,
  RawLot,
  SessionUser,
} from '../types';

const BASE = import.meta.env.VITE_API_BASE ?? '';

export class ApiError extends Error {
  status: number;
  constructor(status: number, message: string) {
    super(message);
    this.status = status;
    this.name = 'ApiError';
  }
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  let res: Response;
  try {
    const token = sessionStorage.getItem('wn_token') ?? localStorage.getItem('wn_token');
    res = await fetch(`${BASE}${path}`, {
      headers: {
        'Content-Type': 'application/json',
        ...(token ? { Authorization: `Bearer ${token}` } : {}),
        ...(init?.headers ?? {}),
      },
      ...init,
    });
  } catch {
    throw new ApiError(0, "Couldn't reach the server.");
  }
  if (!res.ok) {
    let msg = 'Something went wrong. Try again.';
    try {
      const body = (await res.json()) as { error?: string };
      if (body.error) msg = body.error;
    } catch { /* ignore */ }
    throw new ApiError(res.status, msg);
  }
  return res.json() as Promise<T>;
}

// ──────────────────────────────────────────────────────────────────────────────
// Auth
// ──────────────────────────────────────────────────────────────────────────────

export interface SignUpPayload {
  username: string;
  email: string;
  password: string;
  phone?: string;
}

export interface AuthResponse {
  user_id: string;
  username: string;
  email: string;
  role?: string;
  token: string;
}

export interface SignInPayload {
  username: string;
  password: string;
}

export interface ChangePasswordPayload {
  old_password?: string;
  new_password: string;
}

export interface ResetPasswordPayload {
  username: string;
  email?: string;
  new_password: string;
}

export const auth = {
  signUp: (body: SignUpPayload) =>
    request<AuthResponse>('/api/auth/signup', { method: 'POST', body: JSON.stringify(body) }),

  signIn: (body: SignInPayload) =>
    request<AuthResponse>('/api/auth/signin', { method: 'POST', body: JSON.stringify(body) }),

  changePassword: (body: ChangePasswordPayload) =>
    request<{ success: boolean; message: string }>('/api/auth/change-password', {
      method: 'POST',
      body: JSON.stringify(body),
    }),

  resetPassword: (body: ResetPasswordPayload) =>
    request<AuthResponse>('/api/auth/reset-password', {
      method: 'POST',
      body: JSON.stringify(body),
    }),

  updateProfile: (body: { username?: string; email?: string; phone?: string }) =>
    request<SessionUser>('/api/auth/profile', {
      method: 'POST',
      body: JSON.stringify(body),
    }),

  me: () => request<SessionUser>('/api/auth/me'),
};

// ──────────────────────────────────────────────────────────────────────────────
// Items / Lots
// ──────────────────────────────────────────────────────────────────────────────

export const api = {
  hostInfo: () => request<HostInfo>('/api/host-info'),

  items: (query = '') => request<RawLot[]>(`/api/items${query}`),

  item: (id: string) => request<RawLot>(`/api/items/${id}`),

  wonItems: (userId: string) => request<RawLot[]>(`/api/items/won/${userId}`),

  hostItems: (userId: string) => request<RawLot[]>(`/api/items/host/${userId}`),

  createItem: (body: CreateLotRequest) =>
    request<RawLot>('/api/items', { method: 'POST', body: JSON.stringify(body) }),

  closeItem: (id: string) =>
    request<RawLot>(`/api/items/${id}/close`, { method: 'POST', body: '{}' }),

  cancelItem: (id: string) =>
    request<RawLot>(`/api/items/${id}/cancel`, { method: 'POST', body: '{}' }),

  placeBid: (body: PlaceBidRequest) =>
    request<PlaceBidResponse>('/api/bids', { method: 'POST', body: JSON.stringify(body) }),

  upload: async (file: File): Promise<string> => {
    const form = new FormData();
    form.append('file', file);
    const token =
      sessionStorage.getItem('wn_token') ?? localStorage.getItem('wn_token');
    let res: Response;
    try {
      res = await fetch(`${BASE}/api/upload`, {
        method: 'POST',
        body: form,
        headers: token ? { Authorization: `Bearer ${token}` } : {},
      });
    } catch {
      throw new ApiError(0, "Couldn't reach the server.");
    }
    if (!res.ok) throw new ApiError(res.status, 'Upload failed.');
    const data = (await res.json()) as { url: string };
    return data.url;
  },

  deleteItem: (id: string) =>
    request<{ success: boolean; message: string }>(`/api/items/${id}`, { method: 'DELETE' }),

  users: () => request<SessionUser[]>('/api/users'),

  deleteUser: (uid: string) =>
    request<{ success: boolean; message: string }>(`/api/users/${uid}`, { method: 'DELETE' }),

  resetDb: (mode: 'purge_items' | 'factory_reset') =>
    request<{ success: boolean; message: string; token?: string }>('/api/admin/reset-db', {
      method: 'POST',
      body: JSON.stringify({ mode }),
    }),

  stats: () =>
    request<{
      buffer_pool: {
        hits: number;
        misses: number;
        evictions: number;
        dirty_evictions: number;
        hit_rate: number;
      };
      storage: { total_pages: number };
    }>('/api/stats'),

  simulateLiveRecovery: (params?: { item_id?: string; inflight_user?: string; inflight_amount?: number }) =>
    request<LiveRecoverySimResponse>('/api/recovery/simulate-live', {
      method: 'POST',
      body: JSON.stringify(params ?? {}),
    }),
};

export interface LiveRecoverySimResponse {
  status: string;
  item_id: string;
  item_title: string;
  committed_price: number;
  committed_winner: string;
  inflight_user: string;
  inflight_amount: number;
  page_id: number;
  stats: {
    records_analyzed: number;
    records_redone: number;
    records_undone: number;
    clrs_written: number;
    loser_txn_count: number;
  };
  restored_price: number;
  restored_winner: string;
  wal_records: Array<{
    lsn: number;
    prev_lsn: number;
    txn_id: number;
    type: string;
    page_id: number;
    undo_next_lsn: number;
  }>;
}
