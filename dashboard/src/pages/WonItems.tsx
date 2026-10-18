// Won Items – trophy gallery of lots the current user won.

import { useState, useEffect, useCallback } from 'react';
import { useNavigate } from 'react-router-dom';
import { api } from '../lib/api';
import { useSession } from '../context/SessionContext';
import { useStreamEvents } from '../context/StreamContext';
import { normaliseLot } from '../types';
import type { Lot, RawLot } from '../types';
import { Money, EmptyState } from '../components/Primitives';

export function WonItems() {
  const { user } = useSession();
  const navigate = useNavigate();
  const [lots, setLots]       = useState<Lot[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError]     = useState<string | null>(null);

  const fetch = useCallback(async () => {
    if (!user) return;
    try {
      setError(null);
      // Primary: backend won items relational query
      const rawWon = await api.wonItems(user.user_id).catch(() => []);

      // Complementary catalog query to catch immediately closed/expired lots
      let catalogLots: RawLot[] = [];
      try {
        const rawAll = await api.items();
        const now = Date.now();
        catalogLots = rawAll.filter((lot) => {
          const isWinner =
            lot.current_winner === user.user_id ||
            lot.current_winner === user.username;
          const isSettled =
            lot.status === 'CLOSED' ||
            (lot.end_time_ms > 0 && lot.end_time_ms <= now && lot.status === 'OPEN');
          return isWinner && isSettled;
        });
      } catch {}

      // Deduplicate by lot item_id
      const map = new Map<string, RawLot>();
      for (const item of [...rawWon, ...catalogLots]) {
        map.set(item.item_id, item);
      }

      setLots(Array.from(map.values()).map(normaliseLot));
    } catch (err: unknown) {
      setError(err instanceof Error ? err.message : 'Failed to load.');
    } finally {
      setLoading(false);
    }
  }, [user]);

  useEffect(() => {
    setLoading(true);
    fetch();
  }, [fetch]);

  // Real-time synchronization: listen for item closure or new bids
  useStreamEvents(
    useCallback(
      (event) => {
        if (
          event.type === 'item_closed' ||
          event.type === 'new_bid' ||
          event.type === 'item_deleted' ||
          event.type === 'db_reset'
        ) {
          fetch();
        }
      },
      [fetch],
    ),
  );

  // Fast heartbeat timer to catch when a countdown expires
  useEffect(() => {
    const timer = setInterval(fetch, 3000);
    return () => clearInterval(timer);
  }, [fetch]);

  if (!user) {
    return (
      <EmptyState
        title="Sign in to see your wins"
        sub="Your trophy cabinet awaits."
        action="Go to sign in"
        onAction={() => navigate('/')}
      />
    );
  }

  return (
    <>
      <h1 className="display" style={{ fontSize: 24, marginBottom: 24 }}>Won items</h1>

      {loading ? (
        <div style={{ color: 'var(--text-muted)', fontSize: 14 }}>Loading…</div>
      ) : error ? (
        <div>
          <p style={{ color: 'var(--danger)', marginBottom: 12 }}>{error}</p>
          <button className="btn btn-ghost" onClick={fetch}>Retry</button>
        </div>
      ) : lots.length === 0 ? (
        <EmptyState
          icon={
            <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.2}>
              <path d="M6 9H4a2 2 0 01-2-2V5h4M18 9h2a2 2 0 002-2V5h-4"/>
              <path d="M6 9c0 6 6 9 6 9s6-3 6-9"/>
              <path d="M12 18v3M8 21h8"/>
            </svg>
          }
          title="Nothing won yet"
          sub="Head to the floor and place a bid."
          action="Go to the floor"
          onAction={() => navigate('/auction')}
        />
      ) : (
        <div className="won-grid">
          {lots.map((lot) => (
            <WonCard key={lot.id} lot={lot} />
          ))}
        </div>
      )}
    </>
  );
}

function WonCard({ lot }: { lot: Lot }) {
  const img = lot.images[0];
  const closedAt = lot.status === 'CLOSED' ? new Date(lot.endsAt) : null;

  return (
    <div className="won-card">
      {img ? (
        <img
          className="won-card-img"
          src={img}
          alt={lot.title}
          onError={(e) => { (e.currentTarget as HTMLImageElement).style.display = 'none'; }}
        />
      ) : (
        <div
          className="won-card-img"
          style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', color: 'var(--text-faint)', fontSize: 12 }}
        >
          No image
        </div>
      )}
      <div className="won-card-body">
        <div className="won-badge">
          🏆 You won
        </div>
        <h3 style={{ fontWeight: 500, fontSize: 15, lineHeight: 1.3 }}>{lot.title}</h3>
        {lot.currentBid != null && (
          <div>
            <div style={{ fontSize: 11, color: 'var(--text-muted)', marginBottom: 2 }}>Winning bid</div>
            <div className="won-price"><Money value={lot.currentBid} /></div>
          </div>
        )}
        {closedAt && (
          <div style={{ fontSize: 11, color: 'var(--text-muted)' }}>
            Settled {closedAt.toLocaleDateString()} at {closedAt.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}
          </div>
        )}
        <div style={{ fontSize: 12, color: 'var(--text-muted)' }}>
          Host: <strong>{lot.host}</strong>
        </div>
        <div
          style={{
            marginTop: 4,
            padding: '8px 12px',
            background: 'var(--surface-2)',
            borderRadius: 6,
            fontSize: 12,
            color: 'var(--text-muted)',
          }}
        >
          Contact the host to arrange settlement.
        </div>
      </div>
    </div>
  );
}
