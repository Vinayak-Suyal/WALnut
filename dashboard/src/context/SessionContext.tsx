// Session context – stores the signed-in user and persists to storage.

import { createContext, useContext, useState, useEffect, useCallback, type ReactNode } from 'react';
import { auth } from '../lib/api';
import type { SessionUser } from '../types';

interface SessionCtx {
  user: SessionUser | null;
  token: string | null;
  signIn: (user: SessionUser, token: string, remember: boolean) => void;
  signOut: () => void;
  updateUser: (user: SessionUser) => void;
}

const Ctx = createContext<SessionCtx | null>(null);

function loadSession(): { user: SessionUser | null; token: string | null } {
  try {
    const raw = sessionStorage.getItem('wn_session') ?? localStorage.getItem('wn_session');
    if (raw) {
      const parsed = JSON.parse(raw) as { user: SessionUser; token: string };
      return parsed;
    }
  } catch { /* ignore */ }
  return { user: null, token: null };
}

export function SessionProvider({ children }: { children: ReactNode }) {
  const [{ user, token }, setSession] = useState(loadSession);

  const signOut = useCallback(() => {
    sessionStorage.removeItem('wn_session');
    sessionStorage.removeItem('wn_token');
    localStorage.removeItem('wn_session');
    localStorage.removeItem('wn_token');
    setSession({ user: null, token: null });
  }, []);

  const signIn = useCallback((u: SessionUser, t: string, remember: boolean) => {
    const data = JSON.stringify({ user: u, token: t });
    sessionStorage.setItem('wn_session', data);
    sessionStorage.setItem('wn_token', t);
    if (remember) {
      localStorage.setItem('wn_session', data);
      localStorage.setItem('wn_token', t);
    }
    setSession({ user: u, token: t });
  }, []);

  const updateUser = useCallback((u: SessionUser) => {
    setSession((prev) => {
      if (!prev.token) return prev;
      const data = JSON.stringify({ user: u, token: prev.token });
      if (sessionStorage.getItem('wn_session')) sessionStorage.setItem('wn_session', data);
      if (localStorage.getItem('wn_session')) localStorage.setItem('wn_session', data);
      return { user: u, token: prev.token };
    });
  }, []);

  // Validate session on mount against database records
  useEffect(() => {
    if (token) {
      auth.me()
        .then((freshUser) => {
          setSession((prev) => ({ ...prev, user: freshUser }));
        })
        .catch(() => {
          // Token is dead or user does not exist in DB (e.g. wiped DB) -> clear immediately
          signOut();
        });
    }
  }, [token, signOut]);

  // Handle immediate invalidation from real-time events (factory reset or user deletion)
  useEffect(() => {
    const handleInvalidation = (e: Event) => {
      const custom = e as CustomEvent<{ type: string; mode?: string; user_id?: string }>;
      if (custom.detail?.type === 'db_reset' && custom.detail?.mode === 'factory_reset') {
        signOut();
      } else if (custom.detail?.type === 'user_deleted' && custom.detail?.user_id === user?.user_id) {
        signOut();
      }
    };
    window.addEventListener('walnut:session_invalidated', handleInvalidation);
    return () => window.removeEventListener('walnut:session_invalidated', handleInvalidation);
  }, [user, signOut]);

  return <Ctx.Provider value={{ user, token, signIn, signOut, updateUser }}>{children}</Ctx.Provider>;
}

export function useSession() {
  const ctx = useContext(Ctx);
  if (!ctx) throw new Error('useSession must be used inside SessionProvider');
  return ctx;
}
