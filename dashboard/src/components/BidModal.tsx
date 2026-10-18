// Bid Modal – full two-column layout with gallery, subtle top bids table,
// floor increment choice ($1 floor), guard messages, anti-sniping flash,
// and full-resolution lightbox viewer.

import { useState, useEffect, useCallback } from 'react';
import { api } from '../lib/api';
import { useSession } from '../context/SessionContext';
import { useStreamEvents } from '../context/StreamContext';
import { useToast } from '../context/ToastContext';
import {
  Countdown, Money, Receipt,
  labelCategory, labelCondition,
} from './Primitives';
import { ImageLightbox } from './ImageLightbox';
import { normaliseLot } from '../types';
import type { Lot, PlaceBidResponse, BidRejection, StreamEvent } from '../types';

const GUARD_MSG: Record<BidRejection, (min?: number) => string> = {
  HOST_CANNOT_BID: () => 'Hosts cannot bid on their own lots.',
  ALREADY_LEADING: () => 'You already hold the highest bid.',
  BELOW_MINIMUM:   (n) => `Minimum next bid is $${n?.toLocaleString() ?? ''}.`,
  OUTBID_RACE:     (n) => `Another bid landed first. Try $${n?.toLocaleString() ?? ''}.`,
  LOT_CLOSED:      () => 'This auction has ended and is closed.',
  UNKNOWN:         () => "Couldn't place your bid. Try again.",
};

function parseRejection(error: string): BidRejection {
  const lower = error.toLowerCase();
  if (lower.includes('host')) return 'HOST_CANNOT_BID';
  if (lower.includes('already') || lower.includes('leading')) return 'ALREADY_LEADING';
  if (lower.includes('below') || lower.includes('minimum')) return 'BELOW_MINIMUM';
  if (lower.includes('closed') || lower.includes('ended')) return 'LOT_CLOSED';
  if (lower.includes('race') || lower.includes('concurrent')) return 'OUTBID_RACE';
  return 'UNKNOWN';
}

interface Props {
  lot: Lot;
  onClose: () => void;
  onOpenEngine?: (lsn: string) => void;
}

export function BidModal({ lot, onClose, onOpenEngine: _onOpenEngine }: Props) {
  const { user } = useSession();
  const toast = useToast();

  // Internal reactive lot state that updates immediately on bid success or real-time SSE
  const [currentLot, setCurrentLot] = useState<Lot>(lot);

  // Sync when prop lot changes
  useEffect(() => {
    setCurrentLot(lot);
  }, [lot]);

  // Floor value: $1 increment minimum above current bid (or starting price if no bids yet)
  const base       = currentLot.currentBid ?? currentLot.startingPrice;
  const minNext    = currentLot.currentBid == null ? currentLot.startingPrice : currentLot.currentBid + 1;
  const [amount, setAmount]   = useState(minNext);
  const [pending, setPending] = useState(false);
  const [error, setError]     = useState<string | null>(null);
  const [receipt, setReceipt] = useState<PlaceBidResponse | null>(null);
  const [extended, setExtended] = useState(false);
  const [imgIdx, setImgIdx]   = useState(0);
  const [lightboxOpen, setLightboxOpen] = useState(false);

  // Check if auction timer has reached zero
  const isTimeUp = currentLot.endsAt ? new Date(currentLot.endsAt).getTime() <= Date.now() : false;
  const isClosed = currentLot.status !== 'OPEN' || isTimeUp;

  // Real-time synchronization while modal is open
  useStreamEvents(
    useCallback(
      (event: StreamEvent) => {
        if (event.type === 'new_bid' && event.item_id === currentLot.id) {
          if (event.item) {
            const updated = normaliseLot(event.item);
            setCurrentLot(updated);
            const nextMin = (updated.currentBid ?? updated.startingPrice) + 1;
            setAmount((prev) => (prev < nextMin ? nextMin : prev));
          } else {
            api.item(currentLot.id).then((raw) => {
              const updated = normaliseLot(raw);
              setCurrentLot(updated);
              const nextMin = (updated.currentBid ?? updated.startingPrice) + 1;
              setAmount((prev) => (prev < nextMin ? nextMin : prev));
            }).catch(() => {});
          }
        }
      },
      [currentLot.id],
    ),
  );

  // Sync minNext when currentLot updates
  useEffect(() => {
    const updatedMin = currentLot.currentBid == null ? currentLot.startingPrice : currentLot.currentBid + 1;
    setAmount((prev) => (prev < updatedMin ? updatedMin : prev));
  }, [currentLot.currentBid, currentLot.startingPrice]);

  // Trap focus & close on Escape
  useEffect(() => {
    const handler = (e: KeyboardEvent) => { if (e.key === 'Escape' && !lightboxOpen) onClose(); };
    document.addEventListener('keydown', handler);
    document.body.style.overflow = 'hidden';
    return () => {
      document.removeEventListener('keydown', handler);
      document.body.style.overflow = '';
    };
  }, [onClose, lightboxOpen]);

  async function placeBid() {
    if (pending || isClosed) return;
    setError(null);

    // Client-side preflight guards
    if (!user) { setError('Please sign in to bid.'); return; }
    if (currentLot.host === user.user_id) { setError(GUARD_MSG.HOST_CANNOT_BID()); return; }
    if (currentLot.currentWinner === user.user_id) { setError(GUARD_MSG.ALREADY_LEADING()); return; }
    if (amount < minNext) { setError(GUARD_MSG.BELOW_MINIMUM(minNext)); return; }
    if (isClosed) { setError(GUARD_MSG.LOT_CLOSED()); return; }

    setPending(true);
    try {
      const res = await api.placeBid({
        item_id: currentLot.id,
        user_id: user.user_id,
        amount,
      });

      if (res.error) {
        const r = parseRejection(res.error);
        setError(GUARD_MSG[r](res.minNext));
      } else {
        setReceipt(res);
        if (res.extended) setExtended(true);
        toast('Bid placed successfully!', 'success');

        // Immediately update currentLot in place without needing to close & reopen
        if (res.item) {
          const updated = normaliseLot(res.item);
          setCurrentLot(updated);
          const nextMin = (updated.currentBid ?? updated.startingPrice) + 1;
          setAmount(nextMin);
        } else {
          setCurrentLot((prev) => {
            const newBidAmount = amount;
            const updatedPodium = [
              { amount: newBidAmount, user: user.user_id, at: new Date().toISOString() },
              ...(prev.podium || []).filter((p) => p.user !== user.user_id || p.amount !== newBidAmount),
            ].slice(0, 3);
            return {
              ...prev,
              currentBid: newBidAmount,
              currentWinner: user.user_id,
              totalBids: (prev.totalBids || 0) + 1,
              podium: updatedPodium,
            };
          });
          setAmount(amount + 1);
        }
      }
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : "Couldn't reach the server.";
      setError(msg);
    } finally {
      setPending(false);
    }
  }

  const images = lot.images.filter(Boolean);

  // Quick increment convenience chips: computed relative to base
  const quickIncrements = [
    1,
    Math.max(5, Math.round(base * 0.01)),
    Math.max(25, Math.round(base * 0.05)),
    Math.max(100, Math.round(base * 0.1)),
  ];
  const uniqueIncrements = Array.from(new Set(quickIncrements)).slice(0, 4);

  return (
    <>
      <div
        className="modal-backdrop"
        role="dialog"
        aria-modal="true"
        aria-labelledby="bid-modal-title"
        onClick={(e) => { if (e.target === e.currentTarget) onClose(); }}
      >
        <div className="modal">
          <button className="modal-close btn" onClick={onClose} aria-label="Close">✕</button>

          {/* Left – gallery */}
          <section className="modal-gallery">
            {images.length > 0 ? (
              <div
                style={{ position: 'relative', cursor: 'zoom-in' }}
                onClick={() => setLightboxOpen(true)}
                title="Click to enlarge image"
              >
                <img
                  className="modal-main-img"
                  src={images[imgIdx]}
                  alt={currentLot.title}
                  onError={(e) => { (e.currentTarget as HTMLImageElement).src = '/placeholder.svg'; }}
                />
                <div className="zoom-hint-badge">
                  🔍 Enlarge
                </div>
              </div>
            ) : (
              <div
                className="modal-main-img"
                style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', color: 'var(--text-faint)', fontSize: 13, borderRadius: 8, background: 'var(--surface-2)' }}
              >
                No image available
              </div>
            )}
            {images.length > 1 && (
              <div className="modal-thumbs">
                {images.map((src, i) => (
                  <button
                    key={src}
                    className={`modal-thumb-btn${i === imgIdx ? ' active-thumb' : ''}`}
                    onClick={() => setImgIdx(i)}
                    aria-label={`View image ${i + 1}`}
                    aria-pressed={i === imgIdx}
                  >
                    <img src={src} alt="" />
                  </button>
                ))}
              </div>
            )}
            <div style={{ marginTop: 8 }}>
              <div className="lot-badges">
                <span className="badge">{labelCategory(currentLot.category)}</span>
                <span className="badge">{labelCondition(currentLot.condition)}</span>
              </div>
              <h2 id="bid-modal-title" className="modal-title" style={{ marginTop: 8 }}>{currentLot.title}</h2>
              <p style={{ fontSize: 12, color: 'var(--text-muted)', marginTop: 4 }}>
                Listed by <strong>{currentLot.host}</strong>
              </p>
            </div>
          </section>

          {/* Right – bidding info & discrete table */}
          <section className="modal-info">
            <div>
              <div className="hero-label">
                {currentLot.currentBid == null ? 'Starting price' : 'Current bid'}
              </div>
              <div className="hero-bid board">${base.toLocaleString()}</div>
              {currentLot.currentWinner ? (
                <div style={{ fontSize: 13, color: 'var(--text)', marginTop: 4, display: 'flex', alignItems: 'center', gap: 6 }}>
                  <span>Leading:</span>
                  <span className="badge" style={{ background: 'var(--accent-wash)', color: 'var(--accent)', fontWeight: 600 }}>
                    {currentLot.currentWinner}
                  </span>
                  {user && currentLot.currentWinner === user.user_id && (
                    <span style={{ fontSize: 11, color: 'var(--success)', fontWeight: 600 }}>
                      (You are leading)
                    </span>
                  )}
                </div>
              ) : (
                <div style={{ fontSize: 12, color: 'var(--text-muted)', marginTop: 4 }}>
                  No bids placed yet
                </div>
              )}
            </div>

            <div>
              <div className="hero-label">{isClosed ? 'Status' : 'Ends in'}</div>
              {isClosed ? (
                <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginTop: 4 }}>
                  <span className="badge" style={{ background: 'var(--surface-2)', color: 'var(--danger)', borderColor: 'var(--danger)', fontSize: 13, padding: '4px 10px' }}>
                    ⛔ Auction Ended
                  </span>
                </div>
              ) : (
                <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                  <Countdown endsAt={currentLot.endsAt} variant="hero" />
                  {extended && (
                    <span className="extended-flash" style={{ fontSize: 13 }}>
                      +30s extended
                    </span>
                  )}
                </div>
              )}
            </div>

            {/* Discrete Subtle Top Bids Table */}
            <div style={{ marginTop: 6, marginBottom: 8 }}>
              <div style={{ fontSize: 11, fontWeight: 700, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: '0.07em', marginBottom: 8 }}>
                Top Bids Audit Table
              </div>
              {(!currentLot.podium || currentLot.podium.length === 0) ? (
                <div style={{ fontSize: 12, color: 'var(--text-muted)', padding: '10px 14px', background: 'var(--surface)', borderRadius: 6, border: '1px solid var(--border)' }}>
                  No bids recorded yet. Be the first to place a bid!
                </div>
              ) : (
                <div className="table-wrap" style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)' }}>
                  <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 12 }}>
                    <thead>
                      <tr style={{ borderBottom: '1px solid var(--border)', background: 'var(--surface-2)', color: 'var(--text-muted)', fontSize: 11 }}>
                        <th style={{ padding: '6px 10px', textAlign: 'left', width: 44 }}>Place</th>
                        <th style={{ padding: '6px 10px', textAlign: 'left' }}>Bidder</th>
                        <th style={{ padding: '6px 10px', textAlign: 'left' }}>Time</th>
                        <th style={{ padding: '6px 10px', textAlign: 'right' }}>Amount</th>
                      </tr>
                    </thead>
                    <tbody>
                      {currentLot.podium.map((entry, idx) => (
                        <tr key={`${entry.user}-${entry.amount}-${idx}`} style={{ borderBottom: idx < currentLot.podium.length - 1 ? '1px solid var(--border)' : 'none' }}>
                          <td style={{ padding: '7px 10px' }}>
                            <span
                              style={{
                                display: 'inline-block',
                                padding: '1px 6px',
                                borderRadius: 4,
                                fontSize: 11,
                                fontWeight: 700,
                                background: idx === 0 ? 'rgba(230, 160, 30, 0.2)' : idx === 1 ? 'rgba(180, 190, 205, 0.2)' : 'rgba(190, 120, 60, 0.2)',
                                color: idx === 0 ? '#E5A520' : idx === 1 ? '#B0B8C8' : '#C88850',
                              }}
                            >
                              {idx === 0 ? '1st' : idx === 1 ? '2nd' : '3rd'}
                            </span>
                          </td>
                          <td style={{ padding: '7px 10px', fontWeight: idx === 0 ? 600 : 400 }}>
                            {entry.user}
                            {user && entry.user === user.user_id && (
                              <span style={{ marginLeft: 6, fontSize: 10, background: 'var(--accent-wash)', color: 'var(--accent)', padding: '1px 4px', borderRadius: 3 }}>
                                You
                              </span>
                            )}
                          </td>
                          <td style={{ padding: '7px 10px', fontSize: 11, color: 'var(--text-muted)' }}>
                            {entry.at ? new Date(entry.at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }) : '—'}
                          </td>
                          <td style={{ padding: '7px 10px', textAlign: 'right', fontFamily: 'var(--font-num)', fontWeight: 600 }}>
                            ${entry.amount.toLocaleString()}
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              )}
            </div>

            {/* Confirmation Banner for Last Placed Bid */}
            {receipt && (
              <div className="confirm-msg" style={{ marginBottom: 12 }}>
                <span aria-hidden>✓</span>
                <div style={{ flex: 1 }}>
                  <div style={{ fontWeight: 600, fontSize: 13, display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
                    <span>Your bid was accepted by engine!</span>
                    <button
                      type="button"
                      onClick={() => setReceipt(null)}
                      style={{ background: 'none', border: 'none', color: 'var(--text-muted)', cursor: 'pointer', fontSize: 11 }}
                    >
                      Dismiss ✕
                    </button>
                  </div>
                  {receipt.bid_id != null && (
                    <div style={{ marginTop: 2, fontSize: 12 }}>
                      Bid #{receipt.bid_id}
                      {receipt.lsn && <> · <Receipt lsn={receipt.lsn} /></>}
                    </div>
                  )}
                  {extended && (
                    <div className="extended-flash" style={{ marginTop: 2, fontSize: 12 }}>
                      +30s soft-close extended
                    </div>
                  )}
                </div>
              </div>
            )}

            {/* Bidding Controls (Always active when OPEN, allowing consecutive bids) */}
            {!isClosed && (
              <div>
                {/* Minimum floor indication */}
                <div style={{ fontSize: 12, color: 'var(--text-muted)', marginBottom: 8, display: 'flex', justifyContent: 'space-between' }}>
                  <span>Minimum next bid: <strong style={{ color: 'var(--text)' }}><Money value={minNext} /></strong></span>
                  <span style={{ color: 'var(--accent)' }}>$1 floor increment</span>
                </div>

                {/* Quick-increment chips */}
                <div className="bid-chips" style={{ marginBottom: 12 }}>
                  {uniqueIncrements.map((inc) => (
                    <button
                      key={inc}
                      className="chip"
                      onClick={() => setAmount(base + inc)}
                      title={`Add $${inc.toLocaleString()} to current bid`}
                    >
                      +${inc.toLocaleString()}
                    </button>
                  ))}
                </div>

                {/* Custom amount */}
                <div className="bid-input-wrap" style={{ marginBottom: 12 }}>
                  <span className="bid-input-symbol">$</span>
                  <input
                    id="bid-amount"
                    type="number"
                    className="bid-input"
                    value={amount}
                    min={minNext}
                    step={1}
                    onChange={(e) => setAmount(Number(e.target.value))}
                    aria-label="Bid amount"
                    inputMode="numeric"
                  />
                </div>

                {error && (
                  <div className="guard-msg" role="alert">
                    <span aria-hidden>⚠</span>
                    {error}
                  </div>
                )}

                <button
                  className="btn btn-primary btn-full"
                  onClick={placeBid}
                  disabled={pending}
                  style={{ marginTop: 8 }}
                >
                  {pending ? 'Placing bid…' : 'Place bid'}
                </button>
              </div>
            )}

            {isClosed && (
              <div className="guard-msg" style={{ marginTop: 8 }}>
                <span aria-hidden>ⓘ</span>
                This auction lot has ended. No new bids can be submitted.
              </div>
            )}
          </section>
        </div>
      </div>

      {lightboxOpen && (
        <ImageLightbox
          images={images}
          initialIndex={imgIdx}
          onClose={() => setLightboxOpen(false)}
          title={lot.title}
        />
      )}
    </>
  );
}
