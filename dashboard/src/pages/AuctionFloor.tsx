// Auction Floor – primary live auction experience with active-only lot grid,
// real-time SSE synchronization, informative ticker tape, and bid modal.

import { useState, useEffect, useCallback, useRef } from 'react';
import { useOutletContext, useNavigate } from 'react-router-dom';
import { api } from '../lib/api';
import { useStreamEvents } from '../context/StreamContext';
import { normaliseLot } from '../types';
import type { Lot, StreamEvent } from '../types';
import { LotCard, SkeletonCard, EmptyState } from '../components/Primitives';
import { BidModal } from '../components/BidModal';

const CATEGORIES = ['All', 'Electronics', 'Watches', 'Art', 'Collectibles', 'Music'];

interface OutletCtx {
  openEngineAt: (lsn: string) => void;
}

interface TickerMessage {
  id: string;
  lotId?: string;
  text: string;
}

export function AuctionFloor() {
  const { openEngineAt } = useOutletContext<OutletCtx>();
  const navigate = useNavigate();

  const [lots, setLots]           = useState<Lot[]>([]);
  const [loading, setLoading]     = useState(true);
  const [error, setError]         = useState<string | null>(null);
  const [category, setCat]        = useState('All');
  const [search, setSearch]       = useState('');
  const [activeLot, setActiveLot] = useState<Lot | null>(null);
  const [flashIds, setFlashIds]   = useState<Set<string>>(new Set());
  const [ticker, setTicker]       = useState<TickerMessage[]>([]);
  const [, setClock]              = useState(Date.now());
  const flashTimer = useRef<Map<string, ReturnType<typeof setTimeout>>>(new Map());

  // 1-second interval to continuously prune lots that reach 00:00 off the live floor
  useEffect(() => {
    const timer = setInterval(() => setClock(Date.now()), 1000);
    return () => clearInterval(timer);
  }, []);

  const flash = useCallback((id: string) => {
    setFlashIds((s) => new Set(s).add(id));
    if (flashTimer.current.has(id)) clearTimeout(flashTimer.current.get(id));
    const t = setTimeout(() => setFlashIds((s) => { const n = new Set(s); n.delete(id); return n; }), 1200);
    flashTimer.current.set(id, t);
  }, []);

  // Fetch initial items
  const fetchItems = useCallback(async () => {
    try {
      setError(null);
      const raw = await api.items();
      setLots(raw.map(normaliseLot));
    } catch (err: unknown) {
      setError(err instanceof Error ? err.message : 'Failed to load lots.');
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => { fetchItems(); }, [fetchItems]);

  // SSE live updates with atomic podium and lot sync
  const streamStatus = useStreamEvents(
    useCallback(
      (event: StreamEvent) => {
        if (event.type === 'new_bid') {
          const bidEv = event;
          flash(bidEv.item_id);

          if (bidEv.item) {
            const updated = normaliseLot(bidEv.item);
            setLots((prev) => prev.map((l) => (l.id === updated.id ? updated : l)));
            setActiveLot((prev) => (prev?.id === updated.id ? updated : prev));
            const msg: TickerMessage = {
              id: `${bidEv.bid_id || Date.now()}`,
              lotId: updated.id,
              text: `$${bidEv.amount.toLocaleString()} bid on "${updated.title}" by ${bidEv.current_winner}`,
            };
            setTicker((t) => [msg, ...t].slice(0, 20));
          } else {
            api.item(bidEv.item_id).then((raw) => {
              const updated = normaliseLot(raw);
              setLots((prev) => prev.map((l) => (l.id === updated.id ? updated : l)));
              setActiveLot((prev) => (prev?.id === updated.id ? updated : prev));
              const msg: TickerMessage = {
                id: `${bidEv.bid_id || Date.now()}`,
                lotId: updated.id,
                text: `$${bidEv.amount.toLocaleString()} bid on "${updated.title}" by ${bidEv.current_winner}`,
              };
              setTicker((t) => [msg, ...t].slice(0, 20));
            }).catch(() => {
              setLots((prev) =>
                prev.map((lot) => {
                  if (lot.id !== bidEv.item_id) return lot;
                  return {
                    ...lot,
                    currentBid: bidEv.amount,
                    currentWinner: bidEv.current_winner,
                  };
                }),
              );
            });
          }
        }
        if (event.type === 'new_item') {
          api.items().then((raw) => {
            setLots(raw.map(normaliseLot));
          }).catch(() => {});
          setTicker((t) => [{ id: `new_${Date.now()}`, text: '✨ New lot published on floor' }, ...t].slice(0, 20));
        }
        if (event.type === 'item_deleted') {
          const delEv = event;
          setLots((prev) => prev.filter((lot) => lot.id !== delEv.item_id));
          setActiveLot((prev) => (prev?.id === delEv.item_id ? null : prev));
          setTicker((t) => [{ id: `del_${Date.now()}`, text: '🗑 Lot removed from floor' }, ...t].slice(0, 20));
        }
        if (event.type === 'db_reset') {
          fetchItems();
          setActiveLot(null);
          setTicker((t) => [{ id: `rst_${Date.now()}`, text: '🔄 Database reset by admin' }, ...t].slice(0, 20));
        }
      },
      [flash, fetchItems],
    ),
  );

  // Filtered lots: STRICTLY only live items whose clock has not expired
  const now = Date.now();
  const filtered = lots
    .filter((lot) => {
      const isTimeUp = lot.endsAt ? new Date(lot.endsAt).getTime() <= now : false;
      return lot.status === 'OPEN' && !isTimeUp;
    })
    .filter((lot) => {
      if (category !== 'All' && lot.category.toUpperCase() !== category.toUpperCase()) return false;
      if (search && !lot.title.toLowerCase().includes(search.toLowerCase()) && !lot.host.toLowerCase().includes(search.toLowerCase())) return false;
      return true;
    })
    .sort((a, b) => new Date(a.endsAt).getTime() - new Date(b.endsAt).getTime());

  function openModal(id: string) {
    const lot = lots.find((l) => l.id === id);
    if (lot) setActiveLot(lot);
  }

  return (
    <>
      {/* Informative live ticker strip */}
      {ticker.length > 0 && (
        <div className="ticker" aria-label="Live activity" role="marquee">
          <div className="ticker-inner">
            {[...ticker, ...ticker].map((item, i) => (
              <span
                key={`${item.id}_${i}`}
                className="ticker-item"
                style={{ cursor: item.lotId ? 'pointer' : 'default' }}
                onClick={() => { if (item.lotId) openModal(item.lotId); }}
                title={item.lotId ? 'Click to inspect this lot' : undefined}
              >
                <span className="live-dot" aria-hidden style={{ width: 6, height: 6, background: 'var(--accent)', borderRadius: '50%', display: 'inline-block' }} />
                {item.text}
                {i < ticker.length * 2 - 1 && <span className="divider" />}
              </span>
            ))}
          </div>
        </div>
      )}

      {/* Header */}
      <div
        style={{ display: 'flex', alignItems: 'baseline', justifyContent: 'space-between', flexWrap: 'wrap', gap: 16, marginBottom: 20, marginTop: 8 }}
      >
        <div>
          <h1 className="display" style={{ fontSize: 24, margin: 0 }}>Auction floor</h1>
        </div>
        {streamStatus === 'reconnecting' && (
          <div style={{ fontSize: 12, color: 'var(--accent)' }}>⚡ Reconnecting stream…</div>
        )}
      </div>

      {/* Filters + search */}
      <div className="filter-row" style={{ marginBottom: 24 }}>
        {CATEGORIES.map((cat) => (
          <button
            key={cat}
            className={`filter-pill${category === cat ? ' active' : ''}`}
            onClick={() => setCat(cat)}
          >
            {cat}
          </button>
        ))}
        <div style={{ flex: 1 }} />
        <div className="search-box">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={2} strokeLinecap="round" strokeLinejoin="round">
            <circle cx="11" cy="11" r="8" /><line x1="21" y1="21" x2="16.65" y2="16.65" />
          </svg>
          <input
            type="search"
            className="search-input"
            placeholder="Search active lots or hosts…"
            value={search}
            onChange={(e) => setSearch(e.target.value)}
            aria-label="Search lots"
          />
        </div>
      </div>

      {/* Grid */}
      {loading ? (
        <div className="lot-grid">
          {Array.from({ length: 6 }).map((_, i) => <SkeletonCard key={i} />)}
        </div>
      ) : error ? (
        <div style={{ textAlign: 'center', padding: 40 }}>
          <p style={{ color: 'var(--danger)', marginBottom: 12 }}>{error}</p>
          <button className="btn btn-ghost" onClick={fetchItems}>Retry</button>
        </div>
      ) : filtered.length === 0 ? (
        <EmptyState
          icon={<svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.2}><circle cx="12" cy="12" r="9"/><path d="M9 9l6 6M15 9l-6 6"/></svg>}
          title={search || category !== 'All' ? 'No matching live lots' : 'No live lots active on the floor'}
          sub={search || category !== 'All' ? 'Try a different filter or search.' : 'All previously listed items have ended. Be the first to host a new lot.'}
          action="Host a new lot"
          onAction={() => navigate('/host')}
        />
      ) : (
        <div className="lot-grid">
          {filtered.map((lot) => (
            <LotCard
              key={lot.id}
              lot={lot}
              onBid={openModal}
              flash={flashIds.has(lot.id)}
            />
          ))}
        </div>
      )}

      {/* Bid Modal */}
      {activeLot && (
        <BidModal
          lot={activeLot}
          onClose={() => setActiveLot(null)}
          onOpenEngine={(lsn) => { setActiveLot(null); openEngineAt(lsn); }}
        />
      )}
    </>
  );
}
