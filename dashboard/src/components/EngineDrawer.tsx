// Engine Drawer – right-side panel with Trace and Health tabs
// Opens on any page via the Engine toggle in the navbar.

import { useState, useEffect } from 'react';
import { api } from '../lib/api';
import { useStream } from '../context/StreamContext';
import type { Telemetry, LedgerEntry } from '../types';

const TRACE_STEPS = [
  { name: 'Received',      tag: '',       desc: 'Request reaches the server.' },
  { name: 'Lot locked',    tag: 'lock',   desc: 'Lot reserved — nobody else changes it at once.' },
  { name: 'Rules checked', tag: 'rules',  desc: 'Not the host, not already leading, above minimum.' },
  { name: 'Logged',        tag: 'log',    desc: 'Bid written to the Write-Ahead Log first.' },
  { name: 'Page updated',  tag: 'page',   desc: 'Lot page updated in memory (buffer pool).' },
  { name: 'Committed',     tag: 'commit', desc: 'Committed, lock released.' },
  { name: 'Broadcast',     tag: 'live',   desc: 'Pushed to every open screen via SSE.' },
];

interface Props {
  onClose: () => void;
  activeLsn?: string;
  onNavigateEngine?: () => void;
}

export function EngineDrawer({ onClose, activeLsn, onNavigateEngine }: Props) {
  const [tab, setTab] = useState<'trace' | 'health'>('trace');
  const [traceStep, setTraceStep] = useState(-1);
  const [replaying, setReplaying] = useState(false);
  const [telemetry, setTelemetry] = useState<Telemetry | null>(null);
  const { ledger } = useStream();
  const [selectedLsn, setSelectedLsn] = useState<string | undefined>(activeLsn);

  // Auto-start trace when opened with a specific LSN
  useEffect(() => {
    if (activeLsn) {
      setSelectedLsn(activeLsn);
      setTab('trace');
      runTrace();
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [activeLsn]);

  useEffect(() => {
    if (tab === 'health') {
      api.stats().then((s) => {
        setTelemetry({
          recent: [],
          bufferPool: {
            hits: s.buffer_pool.hits,
            misses: s.buffer_pool.misses,
            evictions: s.buffer_pool.evictions,
            hitRatio: s.buffer_pool.hit_rate,
            dirtyPages: s.buffer_pool.dirty_evictions,
          },
          locks: { active: 0, waiting: 0 },
          checkpoint: { lastAt: null, status: 'idle' },
          storage: s.storage,
        });
      }).catch(() => {});
    }
  }, [tab]);

  function runTrace() {
    setTraceStep(0);
    setReplaying(true);
  }

  useEffect(() => {
    if (!replaying) return;
    if (traceStep >= TRACE_STEPS.length) {
      setReplaying(false);
      return;
    }
    const id = setTimeout(() => setTraceStep((s) => s + 1), 400);
    return () => clearTimeout(id);
  }, [replaying, traceStep]);

  function replay() {
    setTraceStep(0);
    setReplaying(true);
  }

  // Trap Escape
  useEffect(() => {
    const handler = (e: KeyboardEvent) => { if (e.key === 'Escape') onClose(); };
    document.addEventListener('keydown', handler);
    return () => document.removeEventListener('keydown', handler);
  }, [onClose]);

  return (
    <>
      <div className="drawer-overlay" onClick={onClose} aria-hidden />
      <aside className="drawer" aria-label="Engine view">
        {/* Header */}
        <div className="drawer-header">
          <span className="drawer-title">⚙ Engine view</span>
          {onNavigateEngine && (
            <button
              className="btn btn-ghost btn-sm"
              onClick={onNavigateEngine}
              style={{ fontSize: 12 }}
            >
              Full view →
            </button>
          )}
          <button className="icon-btn" onClick={onClose} aria-label="Close engine drawer">✕</button>
        </div>

        {/* Tabs */}
        <div className="drawer-tabs" role="tablist">
          {(['trace', 'health'] as const).map((t) => (
            <button
              key={t}
              role="tab"
              aria-selected={tab === t}
              className={`drawer-tab${tab === t ? ' active' : ''}`}
              onClick={() => setTab(t)}
            >
              {t === 'trace' ? 'Bid trace' : 'Health'}
            </button>
          ))}
        </div>

        <div className="drawer-body">
          {tab === 'trace' && (
            <>
              {/* LSN selector from ledger */}
              {ledger.length > 0 && (
                <div>
                  <div style={{ fontSize: 11, fontWeight: 600, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: '0.06em', marginBottom: 8 }}>
                    Recent operations
                  </div>
                  <div className="ledger-list">
                    {ledger.slice(0, 8).map((entry, i) => (
                      <LedgerRow
                        key={i}
                        entry={entry}
                        selected={entry.lsn === selectedLsn}
                        onClick={() => {
                          setSelectedLsn(entry.lsn);
                          replay();
                        }}
                      />
                    ))}
                  </div>
                </div>
              )}

              {/* Trace */}
              <div>
                <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 12 }}>
                  <div style={{ fontSize: 11, fontWeight: 600, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: '0.06em' }}>
                    Bid journey {selectedLsn ? <span style={{ fontFamily: 'var(--font-num)', color: 'var(--text-faint)' }}>#{selectedLsn.split('/')[1]}</span> : ''}
                  </div>
                  <div style={{ display: 'flex', gap: 6 }}>
                    <button className="btn btn-ghost btn-sm" onClick={replay}>↻ Replay</button>
                    {traceStep < 0 && <button className="btn btn-subtle btn-sm" onClick={runTrace}>▶ Run</button>}
                  </div>
                </div>

                {traceStep < 0 ? (
                  <p style={{ fontSize: 13, color: 'var(--text-muted)' }}>
                    Press Run to animate the 7-step bid journey through the engine.
                  </p>
                ) : (
                  <div className="trace-timeline">
                    {TRACE_STEPS.map((step, i) => {
                      const done   = i < traceStep;
                      const active = i === traceStep - 1 && replaying;
                      return (
                        <div key={i} className="trace-step">
                          <div className={`trace-dot${done ? ' done' : active ? ' active' : ''}`}>
                            {done ? '✓' : i + 1}
                          </div>
                          <div className="trace-content">
                            {step.tag && <div className={`trace-tag ${step.tag}`}>{step.tag}</div>}
                            <div className="trace-step-name">{step.name}</div>
                            <div className="trace-step-desc">{step.desc}</div>
                            {done && (
                              <div className="trace-step-time num">
                                +{(i * 0.4 + 0.1).toFixed(1)} ms
                              </div>
                            )}
                          </div>
                        </div>
                      );
                    })}
                  </div>
                )}
              </div>

              {/* Student vs Engine language */}
              {traceStep >= TRACE_STEPS.length && (
                <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 12 }}>
                  <div style={{ fontSize: 11, fontWeight: 600, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: '0.06em', marginBottom: 8 }}>
                    What the student sees
                  </div>
                  <div style={{ fontSize: 13 }}>
                    <div style={{ color: 'var(--success)', fontWeight: 500 }}>✓ Bid placed</div>
                    <div style={{ color: 'var(--text-muted)', marginTop: 4 }}>You're leading.</div>
                  </div>
                </div>
              )}
            </>
          )}

          {tab === 'health' && (
            <>
              <div>
                <div style={{ fontSize: 11, fontWeight: 600, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: '0.06em', marginBottom: 12 }}>
                  Buffer pool
                </div>
                <div className="health-grid">
                  <div className="health-card">
                    <div className="health-card-label">Hit rate</div>
                    <div className="health-card-value num">
                      {telemetry ? `${(telemetry.bufferPool.hitRatio * 100).toFixed(1)}%` : '—'}
                    </div>
                  </div>
                  <div className="health-card">
                    <div className="health-card-label">Dirty pages</div>
                    <div className="health-card-value num">
                      {telemetry?.bufferPool.dirtyPages ?? '—'}
                    </div>
                  </div>
                  <div className="health-card">
                    <div className="health-card-label">Total pages</div>
                    <div className="health-card-value num">
                      {telemetry?.storage?.total_pages ?? '—'}
                    </div>
                  </div>
                  <div className="health-card">
                    <div className="health-card-label">Evictions</div>
                    <div className="health-card-value num">
                      {telemetry?.bufferPool.evictions ?? '—'}
                    </div>
                  </div>
                </div>
              </div>

              <div>
                <div style={{ fontSize: 11, fontWeight: 600, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: '0.06em', marginBottom: 12 }}>
                  Strict 2PL locks
                </div>
                <div className="health-grid">
                  <div className="health-card">
                    <div className="health-card-label">Active</div>
                    <div className="health-card-value num">{telemetry?.locks.active ?? '—'}</div>
                    <div className="health-card-sub">Lock holders</div>
                  </div>
                  <div className="health-card">
                    <div className="health-card-label">Waiting</div>
                    <div className="health-card-value num">{telemetry?.locks.waiting ?? '—'}</div>
                    <div className="health-card-sub">Blocked txns</div>
                  </div>
                </div>
              </div>

              <div>
                <div style={{ fontSize: 11, fontWeight: 600, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: '0.06em', marginBottom: 8 }}>
                  ARIES checkpoint
                </div>
                <div style={{ fontSize: 13, color: 'var(--text-muted)' }}>
                  Status: <span style={{ color: 'var(--text)' }}>{telemetry?.checkpoint.status ?? 'idle'}</span>
                  <br />
                  Last: <span style={{ fontFamily: 'var(--font-num)', fontSize: 12 }}>
                    {telemetry?.checkpoint.lastAt
                      ? new Date(telemetry.checkpoint.lastAt).toLocaleTimeString()
                      : 'No checkpoints yet'}
                  </span>
                </div>
              </div>

              <div>
                <div style={{ fontSize: 11, fontWeight: 600, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: '0.06em', marginBottom: 8 }}>
                  WAL / Recovery
                </div>
                <div style={{ fontSize: 13, color: 'var(--text-muted)' }}>
                  Write-ahead logging ensures committed bids survive a crash.
                  Recovery runs Analysis → Redo → Undo phases on startup.
                </div>
                {onNavigateEngine && (
                  <button
                    className="btn btn-ghost btn-sm"
                    onClick={onNavigateEngine}
                    style={{ marginTop: 12 }}
                  >
                    View full engine explorer →
                  </button>
                )}
              </div>
            </>
          )}
        </div>
      </aside>
    </>
  );
}

function LedgerRow({
  entry,
  selected,
  onClick,
}: {
  entry: LedgerEntry;
  selected: boolean;
  onClick: () => void;
}) {
  const opCls =
    entry.op === 'BID' ? 'bid' :
    entry.op === 'CREATE_ITEM' ? 'create' : 'close';

  const stCls = entry.status === 'COMMITTED' ? 'committed' : 'aborted';

  return (
    <div
      className="ledger-row"
      onClick={onClick}
      style={selected ? { borderColor: 'var(--accent)', background: 'var(--accent-wash)' } : {}}
    >
      <span className={`ledger-op ${opCls}`}>{entry.op}</span>
      <span className={`ledger-status ${stCls}`}>{entry.status}</span>
      <span className="ledger-lsn">#{entry.lsn.split('/')[1] ?? entry.lsn}</span>
      <span className="ledger-time">
        {new Date(entry.at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })}
      </span>
    </div>
  );
}
