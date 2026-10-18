// Shared primitive components

import { useEffect, useState } from 'react';
import type { Lot, PodiumEntry } from '../types';

// ──────────────────────────────────────────────────────────────────────────────
// Label helpers
// ──────────────────────────────────────────────────────────────────────────────

const CONDITION_LABEL: Record<string, string> = {
  NEW: 'New', LIKE_NEW: 'Like new', GOOD: 'Good', FAIR: 'Fair',
  MINT: 'Mint', EXCELLENT: 'Excellent',
};

const CATEGORY_LABEL: Record<string, string> = {
  ELECTRONICS: 'Electronics', ART: 'Art', WATCHES: 'Watches',
  COLLECTIBLES: 'Collectibles', MUSIC: 'Music', GENERAL: 'General',
};

export const labelCondition = (c: string) => CONDITION_LABEL[c] ?? c;
export const labelCategory  = (c: string) => CATEGORY_LABEL[c] ?? c;

// ──────────────────────────────────────────────────────────────────────────────
// Money
// ──────────────────────────────────────────────────────────────────────────────

export function Money({ value, className = '' }: { value: number; className?: string }) {
  const text = new Intl.NumberFormat('en-US', { maximumFractionDigits: 0 }).format(value);
  return <span className={`num ${className}`}>${text}</span>;
}

export function Receipt({ lsn }: { lsn: string }) {
  const short = lsn.includes('/') ? lsn.split('/')[1] : lsn;
  return <span className="num">#{short}</span>;
}

// ──────────────────────────────────────────────────────────────────────────────
// Countdown
// ──────────────────────────────────────────────────────────────────────────────

function formatMs(ms: number) {
  const total = Math.max(0, Math.floor(ms / 1000));
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  const mm = String(m).padStart(2, '0');
  const ss = String(s).padStart(2, '0');
  return h > 0 ? `${String(h).padStart(2, '0')}:${mm}:${ss}` : `${mm}:${ss}`;
}

export function Countdown({
  endsAt,
  offsetMs = 0,
  variant = 'card',
}: {
  endsAt: string;
  offsetMs?: number;
  variant?: 'card' | 'hero';
}) {
  const [now, setNow] = useState(() => Date.now() + offsetMs);

  useEffect(() => {
    const tick = variant === 'hero' ? 100 : 1000;
    const id = window.setInterval(() => setNow(Date.now() + offsetMs), tick);
    return () => window.clearInterval(id);
  }, [offsetMs, variant]);

  const remaining = new Date(endsAt).getTime() - now;
  const urgent  = remaining <= 15_000 && remaining > 0;
  const closed  = remaining <= 0;

  const cls =
    variant === 'hero'
      ? `countdown-hero${urgent ? ' urgent' : ''}`
      : `countdown-card${urgent ? ' urgent' : ''}`;

  return (
    <span
      className={cls}
      aria-label={closed ? 'Auction closed' : `Ends in ${formatMs(remaining)}`}
    >
      {closed ? '00:00' : formatMs(remaining)}
    </span>
  );
}

// ──────────────────────────────────────────────────────────────────────────────
// ConnectionPill
// ──────────────────────────────────────────────────────────────────────────────

type ConnStatus = 'connecting' | 'open' | 'reconnecting' | 'offline';

const PILL_LABEL: Record<ConnStatus, string> = {
  connecting: 'Connecting',
  open: 'Live',
  reconnecting: 'Reconnecting',
  offline: 'Offline',
};

export function ConnectionPill({ status }: { status: ConnStatus }) {
  return (
    <div className={`conn-pill ${status}`} title={`Server: ${status}`}>
      <span className="live-pulse-beacon" aria-hidden />
      <span className="conn-label">{status === 'open' ? 'LIVE' : PILL_LABEL[status]}</span>
    </div>
  );
}

// ──────────────────────────────────────────────────────────────────────────────
// Podium
// ──────────────────────────────────────────────────────────────────────────────

const RANK_CLS = ['gold', 'silver', 'bronze'];
const RANK_LABELS = ['1st', '2nd', '3rd'];

export function Podium({ entries }: { entries: PodiumEntry[] }) {
  if (!entries.length) {
    return <p style={{ fontSize: 13, color: 'var(--text-muted)' }}>No bids yet.</p>;
  }
  return (
    <div className="podium">
      {entries.slice(0, 3).map((e, i) => (
        <div key={i} className="podium-row">
          <span className={`podium-rank ${RANK_CLS[i]}`}>{RANK_LABELS[i]}</span>
          <span className="podium-amount num">${e.amount.toLocaleString()}</span>
          <span className="podium-user">{e.user}</span>
          <span className="podium-time">
            {new Date(e.at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}
          </span>
        </div>
      ))}
    </div>
  );
}

// ──────────────────────────────────────────────────────────────────────────────
// EmptyState
// ──────────────────────────────────────────────────────────────────────────────

export function EmptyState({
  icon,
  title,
  sub,
  action,
  onAction,
}: {
  icon?: React.ReactNode;
  title: string;
  sub?: string;
  action?: string;
  onAction?: () => void;
}) {
  return (
    <div className="empty-state">
      {icon && <div className="empty-icon">{icon}</div>}
      <h2 className="empty-title">{title}</h2>
      {sub && <p className="empty-sub">{sub}</p>}
      {action && (
        <button className="btn btn-primary" onClick={onAction}>
          {action}
        </button>
      )}
    </div>
  );
}

// ──────────────────────────────────────────────────────────────────────────────
// SkeletonCard
// ──────────────────────────────────────────────────────────────────────────────

export function SkeletonCard() {
  return (
    <div className="lot-card" style={{ gap: 0, padding: 0 }}>
      <div className="skeleton" style={{ width: '100%', aspectRatio: '4/3' }} />
      <div style={{ padding: '16px', display: 'flex', flexDirection: 'column', gap: 8 }}>
        <div className="skeleton" style={{ height: 12, width: '60%' }} />
        <div className="skeleton" style={{ height: 16, width: '80%' }} />
        <div className="skeleton" style={{ height: 24, width: '40%' }} />
        <div className="skeleton" style={{ height: 40, marginTop: 8, borderRadius: 4 }} />
      </div>
    </div>
  );
}

// ──────────────────────────────────────────────────────────────────────────────
// LotCard
// ──────────────────────────────────────────────────────────────────────────────

export function LotCard({
  lot,
  onBid,
  flash = false,
}: {
  lot: Lot;
  onBid: (id: string) => void;
  flash?: boolean;
}) {
  const [imgIdx, setImgIdx] = useState(0);
  const isTimeUp = lot.endsAt ? new Date(lot.endsAt).getTime() <= Date.now() : false;
  const open     = lot.status === 'OPEN' && !isTimeUp;
  const price    = lot.currentBid ?? lot.startingPrice;
  const images   = lot.images.filter(Boolean);

  return (
    <article
      className={`lot-card${flash ? ' flashing' : ''}`}
      onClick={() => onBid(lot.id)}
    >
      <div className="lot-media">
        {images.length > 0 ? (
          <img
            className="lot-main-img"
            src={images[imgIdx] || '/placeholder.svg'}
            alt={lot.title}
            loading="lazy"
            onError={(e) => { (e.currentTarget as HTMLImageElement).src = '/placeholder.svg'; }}
          />
        ) : (
          <div
            className="lot-main-img"
            style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', color: 'var(--text-faint)', fontSize: 12 }}
          >
            No image
          </div>
        )}
        {images.length > 1 && (
          <div className="lot-thumbs">
            {images.slice(1, 3).map((src, i) => (
              <img
                key={src}
                className="lot-thumb"
                src={src}
                alt=""
                loading="lazy"
                onClick={(e) => { e.stopPropagation(); setImgIdx(i + 1); }}
                onError={(e) => { (e.currentTarget as HTMLImageElement).style.display = 'none'; }}
              />
            ))}
          </div>
        )}
      </div>

      <div className="lot-body">
        <div className="lot-badges">
          <span className="badge">{labelCategory(lot.category)}</span>
          <span className="badge">{labelCondition(lot.condition)}</span>
          {open ? (
            <span className="badge live">
              <span className="live-dot" aria-hidden />
              Live
            </span>
          ) : (
            <span className="badge settled" style={{ background: 'var(--surface-2)', color: 'var(--text-muted)' }}>
              Ended
            </span>
          )}
        </div>

        <h3 className="lot-title">{lot.title}</h3>

        <div className="lot-pricing">
          <div className="lot-price-block">
            <div className="price-label">{lot.currentBid == null ? 'Starting at' : 'Current bid'}</div>
            <Money value={price} className="lot-price" />
          </div>
          {open && (
            <div style={{ textAlign: 'right' }}>
              <div className="price-label">Ends in</div>
              <Countdown endsAt={lot.endsAt} />
            </div>
          )}
        </div>

        {lot.currentWinner && (
          <div className="lot-leader">Leading: {lot.currentWinner}</div>
        )}

        <button
          className="btn btn-primary bid-now-btn"
          onClick={(e) => { e.stopPropagation(); onBid(lot.id); }}
        >
          {open ? 'Bid now' : 'View lot'}
        </button>
      </div>
    </article>
  );
}

// ──────────────────────────────────────────────────────────────────────────────
// Password Input with show/hide eye toggle
// ──────────────────────────────────────────────────────────────────────────────

export function PasswordInput({
  id,
  value,
  onChange,
  placeholder = '••••••••',
  autoComplete,
  className = '',
  required,
}: {
  id?: string;
  value: string;
  onChange: (e: React.ChangeEvent<HTMLInputElement>) => void;
  placeholder?: string;
  autoComplete?: string;
  className?: string;
  required?: boolean;
}) {
  const [show, setShow] = useState(false);

  return (
    <div className="password-input-wrap">
      <input
        id={id}
        type={show ? 'text' : 'password'}
        className={`form-input ${className}`}
        value={value}
        onChange={onChange}
        placeholder={placeholder}
        autoComplete={autoComplete}
        required={required}
      />
      <button
        type="button"
        className="password-toggle-btn"
        onClick={() => setShow((s) => !s)}
        aria-label={show ? 'Hide password' : 'View password'}
        title={show ? 'Hide password' : 'View password'}
        tabIndex={-1}
      >
        {show ? (
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} width="18" height="18">
            <path d="M17.94 17.94A10.07 10.07 0 0112 20c-7 0-11-8-11-8a18.45 18.45 0 015.06-5.94M9.9 4.24A9.12 9.12 0 0112 4c7 0 11 8 11 8a18.5 18.5 0 01-2.16 3.19m-6.72-1.07a3 3 0 11-4.24-4.24" strokeLinecap="round" strokeLinejoin="round"/>
            <line x1="1" y1="1" x2="23" y2="23" strokeLinecap="round" strokeLinejoin="round"/>
          </svg>
        ) : (
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} width="18" height="18">
            <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z" strokeLinecap="round" strokeLinejoin="round"/>
            <circle cx="12" cy="12" r="3"/>
          </svg>
        )}
      </button>
    </div>
  );
}
