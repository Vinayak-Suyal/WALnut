// SSE stream context – one shared connection per tab.
// Provides real-time auction updates to the entire component tree.

import {
  createContext,
  useContext,
  useEffect,
  useRef,
  useState,
  useCallback,
  type ReactNode,
} from 'react';
import type { StreamEvent, LedgerEntry } from '../types';

type ConnStatus = 'connecting' | 'open' | 'reconnecting' | 'offline';

interface StreamCtx {
  status: ConnStatus;
  ledger: LedgerEntry[];
  subscribe: (handler: (e: StreamEvent) => void) => () => void;
}

const Ctx = createContext<StreamCtx | null>(null);

const MAX_LEDGER = 50;

export function StreamProvider({ children }: { children: ReactNode }) {
  const [status, setStatus] = useState<ConnStatus>('connecting');
  const [ledger, setLedger] = useState<LedgerEntry[]>([]);
  const handlers = useRef<Set<(e: StreamEvent) => void>>(new Set());

  const subscribe = useCallback((h: (e: StreamEvent) => void) => {
    handlers.current.add(h);
    return () => { handlers.current.delete(h); };
  }, []);

  useEffect(() => {
    let es: EventSource | null = null;
    let retryCount = 0;
    let timer: ReturnType<typeof setTimeout> | undefined;
    let stopped = false;

    function connect() {
      es = new EventSource('/api/stream');

      es.onopen = () => {
        retryCount = 0;
        setStatus('open');
      };

      function forward(type: StreamEvent['type']) {
        return (ev: MessageEvent) => {
          try {
            const data = JSON.parse(ev.data as string) as Record<string, unknown>;
            const event = { type, ...data } as StreamEvent;
            handlers.current.forEach((h) => h(event));

            // Update rolling ledger for bids
            if (type === 'new_bid') {
              const entry: LedgerEntry = {
                lsn: `0/${Math.floor(Math.random() * 0xFFFFFF).toString(16).toUpperCase()}`,
                txId: `T-${Date.now() % 10000}`,
                op: 'BID',
                status: 'COMMITTED',
                at: new Date().toISOString(),
              };
              setLedger((prev) => [entry, ...prev].slice(0, MAX_LEDGER));
            }
            if (type === 'new_item') {
              const entry: LedgerEntry = {
                lsn: `0/${Math.floor(Math.random() * 0xFFFFFF).toString(16).toUpperCase()}`,
                txId: `T-${Date.now() % 10000}`,
                op: 'CREATE_ITEM',
                status: 'COMMITTED',
                at: new Date().toISOString(),
              };
              setLedger((prev) => [entry, ...prev].slice(0, MAX_LEDGER));
            }
            if (type === 'item_deleted') {
              const entry: LedgerEntry = {
                lsn: `0/${Math.floor(Math.random() * 0xFFFFFF).toString(16).toUpperCase()}`,
                txId: `T-${Date.now() % 10000}`,
                op: 'CLOSE_ITEM',
                status: 'COMMITTED',
                at: new Date().toISOString(),
              };
              setLedger((prev) => [entry, ...prev].slice(0, MAX_LEDGER));
            }
            if (type === 'user_deleted' || type === 'db_reset') {
              window.dispatchEvent(new CustomEvent('walnut:session_invalidated', { detail: event }));
            }
          } catch { /* ignore malformed */ }
        };
      }

      es.addEventListener('new_bid', forward('new_bid'));
      es.addEventListener('new_item', forward('new_item'));
      es.addEventListener('item_deleted', forward('item_deleted'));
      es.addEventListener('item_closed', forward('item_closed'));
      es.addEventListener('user_deleted', forward('user_deleted'));
      es.addEventListener('db_reset', forward('db_reset'));

      es.onerror = () => {
        es?.close();
        if (stopped) return;
        setStatus(retryCount > 3 ? 'offline' : 'reconnecting');
        retryCount += 1;
        const delay = Math.min(1000 * 2 ** retryCount, 15_000);
        timer = setTimeout(connect, delay);
      };
    }

    connect();

    return () => {
      stopped = true;
      es?.close();
      clearTimeout(timer);
    };
  }, []);

  return <Ctx.Provider value={{ status, ledger, subscribe }}>{children}</Ctx.Provider>;
}

export function useStream() {
  const ctx = useContext(Ctx);
  if (!ctx) throw new Error('useStream must be used inside StreamProvider');
  return ctx;
}

/** Convenience hook to listen for events and also get status */
export function useStreamEvents(handler: (e: StreamEvent) => void) {
  const { status, subscribe } = useStream();
  const ref = useRef(handler);
  ref.current = handler;

  useEffect(() => {
    return subscribe((e) => ref.current(e));
  }, [subscribe]);

  return status;
}
