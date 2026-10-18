import { useState, useEffect, useMemo } from 'react';
import { useNavigate } from 'react-router-dom';
import { api, type LiveRecoverySimResponse } from '../lib/api';
import { useStream } from '../context/StreamContext';
import { useSession } from '../context/SessionContext';
import type { LedgerEntry, RawLot } from '../types';

interface Stats {
  buffer_pool: {
    hits: number;
    misses: number;
    evictions: number;
    dirty_evictions: number;
    hit_rate: number;
  };
  storage: { total_pages: number };
}

// ──────────────────────────────────────────────────────────────────────────────
// 1. BUFFER POOL CLOCK REPLACEMENT VISUALIZER (Interactive Ring & Needle)
// ──────────────────────────────────────────────────────────────────────────────

interface BufferFrame {
  id: number;
  pageId: number | null;
  refBit: number;
  dirty: boolean;
  pinCount: number;
}

const INITIAL_FRAMES: BufferFrame[] = [
  { id: 0, pageId: 1,  refBit: 1, dirty: false, pinCount: 0 },
  { id: 1, pageId: 2,  refBit: 0, dirty: true,  pinCount: 0 },
  { id: 2, pageId: 5,  refBit: 1, dirty: false, pinCount: 1 },
  { id: 3, pageId: 3,  refBit: 0, dirty: false, pinCount: 0 },
  { id: 4, pageId: null,refBit: 0,dirty: false, pinCount: 0 },
  { id: 5, pageId: 8,  refBit: 1, dirty: true,  pinCount: 0 },
  { id: 6, pageId: 4,  refBit: 0, dirty: false, pinCount: 0 },
  { id: 7, pageId: 7,  refBit: 1, dirty: false, pinCount: 0 },
];

function ClockBufferPoolVisualizer({ stats }: { stats: Stats | null }) {
  const [frames, setFrames] = useState<BufferFrame[]>(INITIAL_FRAMES);
  const [hand, setHand] = useState(0);
  const [victimFrame, setVictimFrame] = useState<number | null>(null);
  const [hitFrame, setHitFrame] = useState<number | null>(null);
  const [isSweeping, setIsSweeping] = useState(false);
  const [logMsg, setLogMsg] = useState('Clock hand pointing to Frame 0. Ready for victim scan or page access.');

  const hitRate = stats ? (stats.buffer_pool.hit_rate * 100).toFixed(1) : '0.0';
  const totalIO = stats ? stats.buffer_pool.hits + stats.buffer_pool.misses : 0;

  // Single step clock replacement
  function stepClock() {
    setHitFrame(null);

    // If currently resting on a previously selected victim, advance to the next frame to continue search
    let targetHand = hand;
    if (victimFrame === hand) {
      targetHand = (hand + 1) % 8;
      setHand(targetHand);
      setVictimFrame(null);
    } else {
      setVictimFrame(null);
    }

    const current = frames[targetHand];

    if (current.pageId === null) {
      setVictimFrame(targetHand);
      setLogMsg(`Frame F${targetHand} is empty. Selected as target frame.`);
      return true;
    }

    if (current.refBit === 1) {
      setFrames((prev) =>
        prev.map((f, i) => (i === targetHand ? { ...f, refBit: 0 } : f))
      );
      const nextHand = (targetHand + 1) % 8;
      setHand(nextHand);
      setLogMsg(`Frame F${targetHand} had Ref=1 (Second Chance). Cleared to Ref=0. Hand advanced to F${nextHand}.`);
      return false;
    }

    if (current.pinCount > 0) {
      const nextHand = (targetHand + 1) % 8;
      setHand(nextHand);
      setLogMsg(`Frame F${targetHand} has Ref=0 but is Pinned (active txn). Skipping to F${nextHand}.`);
      return false;
    }

    // Victim found
    setVictimFrame(targetHand);
    setLogMsg(`Victim Found! Frame F${targetHand} (Page P-${current.pageId}, Ref=0) selected for eviction${current.dirty ? ' [Forced WAL Flush required]' : ''}.`);
    return true;
  }

  function startAutoSweep() {
    if (isSweeping) {
      setIsSweeping(false);
      return;
    }
    // If already on a victim, advance past it first so it doesn't immediately halt on the same frame
    if (victimFrame === hand) {
      const nextHand = (hand + 1) % 8;
      setHand(nextHand);
      setVictimFrame(null);
    }
    setIsSweeping(true);
  }

  // Auto-sweep until victim found
  useEffect(() => {
    if (!isSweeping) return;
    const timer = setTimeout(() => {
      const current = frames[hand];
      if (current.pageId === null || (current.refBit === 0 && current.pinCount === 0)) {
        setVictimFrame(hand);
        setLogMsg(`Victim Found! Frame F${hand} (Page P-${current.pageId}, Ref=0) selected for eviction${current.dirty ? ' [Forced WAL Flush required]' : ''}.`);
        setIsSweeping(false);
      } else {
        stepClock();
      }
    }, 450);
    return () => clearTimeout(timer);
  }, [isSweeping, hand, frames]);

  // Evict the currently selected victim and replace with a simulated new page
  function evictAndReplace() {
    if (victimFrame === null) return;
    const vId = victimFrame;
    const oldPage = frames[vId].pageId;
    const newPageNum = Math.floor(Math.random() * 20) + 10;

    setFrames((prev) =>
      prev.map((f, i) =>
        i === vId
          ? {
              ...f,
              pageId: newPageNum,
              refBit: 1,
              dirty: false,
              pinCount: 0,
            }
          : f
      )
    );
    const nextHand = (vId + 1) % 8;
    setHand(nextHand);
    setVictimFrame(null);
    setHitFrame(vId);
    setLogMsg(`💥 Evicted Page P-${oldPage} from Frame F${vId}! Loaded fresh Page P-${newPageNum} (Ref=1). Clock hand advanced to F${nextHand}.`);
  }

  // Request a page (Cache hit or miss)
  function requestPage(targetPage: number) {
    setIsSweeping(false);
    setVictimFrame(null);
    setHitFrame(null);

    const foundIdx = frames.findIndex((f) => f.pageId === targetPage);
    if (foundIdx !== -1) {
      setHitFrame(foundIdx);
      setFrames((prev) =>
        prev.map((f, i) => (i === foundIdx ? { ...f, refBit: 1 } : f))
      );
      setLogMsg(`⚡ Cache HIT! Page P-${targetPage} is present in Frame F${foundIdx}. Reference bit set to 1.`);
    } else {
      let victim = hand;
      let newFrames = [...frames];
      while (newFrames[victim].pageId !== null && (newFrames[victim].refBit === 1 || newFrames[victim].pinCount > 0)) {
        if (newFrames[victim].refBit === 1) {
          newFrames[victim] = { ...newFrames[victim], refBit: 0 };
        }
        victim = (victim + 1) % 8;
      }
      newFrames[victim] = {
        id: victim,
        pageId: targetPage,
        refBit: 1,
        dirty: false,
        pinCount: 0,
      };
      setFrames(newFrames);
      setVictimFrame(victim);
      setHand((victim + 1) % 8);
      setLogMsg(`💥 Cache MISS for Page P-${targetPage}! Evicted victim in Frame F${victim}. Page P-${targetPage} loaded into F${victim}. Hand advanced to F${(victim + 1) % 8}.`);
    }
  }

  function resetFrames() {
    setIsSweeping(false);
    setFrames(INITIAL_FRAMES);
    setHand(0);
    setVictimFrame(null);
    setHitFrame(null);
    setLogMsg('Buffer pool reset to standard state.');
  }

  // 8 Clock Frame Positions (Center: 230, 190, Radius: 125)
  const CLOCK_COORDS = [
    { cx: 230, cy: 65, label: 'F0' },
    { cx: 318, cy: 101, label: 'F1' },
    { cx: 355, cy: 190, label: 'F2' },
    { cx: 318, cy: 279, label: 'F3' },
    { cx: 230, cy: 315, label: 'F4' },
    { cx: 142, cy: 279, label: 'F5' },
    { cx: 105, cy: 190, label: 'F6' },
    { cx: 142, cy: 101, label: 'F7' },
  ];

  const currentFrame = frames[hand];

  return (
    <div className="card" style={{ padding: 24, marginBottom: 28 }}>
      {/* Title & Toolbar */}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', flexWrap: 'wrap', gap: 14, marginBottom: 16 }}>
        <div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
            <h2 className="section-title" style={{ margin: 0, fontSize: 17 }}>
              Buffer Pool · Clock Replacement Algorithm (Second Chance)
            </h2>
            <span
              style={{
                fontSize: 10,
                fontWeight: 700,
                padding: '3px 9px',
                borderRadius: 12,
                background: 'rgba(230, 160, 30, 0.12)',
                color: 'var(--accent)',
                border: '1px solid var(--accent)',
                textTransform: 'uppercase',
                letterSpacing: 0.5,
              }}
            >
              CIRCULAR BUFFER RING
            </span>
          </div>
          <p style={{ fontSize: 12, color: 'var(--text-muted)', margin: '4px 0 0 0' }}>
            Interactive 8-frame circular ring. Clock hand sweeps frames clearing reference bits (1 → 0) until an unpinned victim with Ref=0 is selected.
          </p>
        </div>

        {/* Action Controls */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap' }}>
          <button
            className="btn btn-ghost btn-sm"
            onClick={stepClock}
            disabled={isSweeping}
            style={{ fontSize: 12, padding: '6px 12px' }}
            title="Step clock hand forward by 1 frame"
          >
            ▶ Step (1 Chance)
          </button>
          <button
            className={`btn btn-sm ${isSweeping ? 'btn-danger' : 'btn-primary'}`}
            onClick={startAutoSweep}
            style={{ fontSize: 12, padding: '6px 14px' }}
          >
            {isSweeping ? '⏸ Stop Sweep' : '⚡ Auto-Find Victim'}
          </button>
          {victimFrame !== null && (
            <button
              className="btn btn-sm"
              onClick={evictAndReplace}
              style={{
                fontSize: 12,
                padding: '6px 12px',
                background: 'rgba(239, 68, 68, 0.2)',
                border: '1px solid var(--danger)',
                color: 'var(--danger)',
                fontWeight: 700,
                cursor: 'pointer',
              }}
              title="Evict this victim page and load a fresh page into this frame"
            >
              💥 Evict & Replace F{victimFrame} (Page P-{frames[victimFrame]?.pageId})
            </button>
          )}
          <button
            className="btn btn-ghost btn-sm"
            onClick={resetFrames}
            style={{ fontSize: 12, padding: '6px 10px' }}
            title="Reset frames"
          >
            ↺ Reset
          </button>
        </div>
      </div>

      {/* Live I/O Metrics Banner */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(130px, 1fr))', gap: 12, marginBottom: 18 }}>
        <div style={{ background: 'var(--surface-2)', padding: '10px 14px', borderRadius: 8, border: '1px solid var(--border)' }}>
          <div style={{ fontSize: 11, color: 'var(--text-muted)', textTransform: 'uppercase', fontFamily: 'var(--font-num)' }}>Hit Rate</div>
          <div style={{ fontSize: 20, fontWeight: 700, color: 'var(--accent)', marginTop: 2, fontFamily: 'var(--font-num)' }}>{hitRate}%</div>
          <div style={{ fontSize: 11, color: 'var(--text-faint)', marginTop: 2 }}>{stats?.buffer_pool.hits ?? 0} hits / {totalIO} total</div>
        </div>
        <div style={{ background: 'var(--surface-2)', padding: '10px 14px', borderRadius: 8, border: '1px solid var(--border)' }}>
          <div style={{ fontSize: 11, color: 'var(--text-muted)', textTransform: 'uppercase', fontFamily: 'var(--font-num)' }}>Evictions</div>
          <div style={{ fontSize: 20, fontWeight: 700, color: 'var(--text)', marginTop: 2, fontFamily: 'var(--font-num)' }}>{stats?.buffer_pool.evictions ?? 0}</div>
          <div style={{ fontSize: 11, color: 'var(--text-faint)', marginTop: 2 }}>Victims removed</div>
        </div>
        <div style={{ background: 'var(--surface-2)', padding: '10px 14px', borderRadius: 8, border: '1px solid var(--border)' }}>
          <div style={{ fontSize: 11, color: 'var(--text-muted)', textTransform: 'uppercase', fontFamily: 'var(--font-num)' }}>Dirty Flushes</div>
          <div style={{ fontSize: 20, fontWeight: 700, color: '#f59e0b', marginTop: 2, fontFamily: 'var(--font-num)' }}>{stats?.buffer_pool.dirty_evictions ?? 0}</div>
          <div style={{ fontSize: 11, color: 'var(--text-faint)', marginTop: 2 }}>Forced WAL flushes</div>
        </div>
        <div style={{ background: 'var(--surface-2)', padding: '10px 14px', borderRadius: 8, border: '1px solid var(--border)' }}>
          <div style={{ fontSize: 11, color: 'var(--text-muted)', textTransform: 'uppercase', fontFamily: 'var(--font-num)' }}>Total Pages</div>
          <div style={{ fontSize: 20, fontWeight: 700, color: 'var(--text)', marginTop: 2, fontFamily: 'var(--font-num)' }}>{stats?.storage.total_pages ?? 3}</div>
          <div style={{ fontSize: 11, color: 'var(--text-faint)', marginTop: 2 }}>In walnut.db file</div>
        </div>
      </div>

      {/* Page Access Simulation Bar */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap', marginBottom: 18, background: 'var(--surface)', padding: '10px 14px', borderRadius: 8, border: '1px solid var(--border)' }}>
        <span style={{ fontSize: 12, fontWeight: 600, color: 'var(--text-muted)' }}>Simulate Page Access:</span>
        {[1, 2, 4, 6, 9].map((pid) => (
          <button
            key={pid}
            className="chip"
            onClick={() => requestPage(pid)}
            style={{ fontSize: 11, padding: '4px 10px', height: 26 }}
          >
            {frames.some((f) => f.pageId === pid) ? `Access P-${pid} (Hit)` : `Request P-${pid} (Miss)`}
          </button>
        ))}
      </div>

      {/* Main Grid: Circular SVG Clock Left + Live Diagnostics Right */}
      <div className="wfg-grid">
        {/* LEFT: Circular Clock Face SVG */}
        <div className="clock-svg-wrap">
          <svg className="wfg-svg" viewBox="0 0 460 380">
            <defs>
              <marker id="clock-ah" markerWidth="8" markerHeight="6" refX="7" refY="3" orient="auto">
                <polygon points="0 0, 8 3, 0 6" fill="var(--accent)" />
              </marker>
              <filter id="clock-glow" x="-20%" y="-20%" width="140%" height="140%">
                <feGaussianBlur stdDeviation="3" result="blur" />
                <feComposite in="SourceGraphic" in2="blur" operator="over" />
              </filter>
            </defs>

            {/* Circular Orbit Ring */}
            <circle cx="230" cy="190" r="125" fill="none" stroke="rgba(255, 255, 255, 0.08)" strokeDasharray="6 4" strokeWidth="1.5" />

            {/* Rotating Clock Needle Hand */}
            <line
              x1="230"
              y1="190"
              x2="230"
              y2="92"
              className="clock-hand-line"
              style={{
                transform: `rotate(${hand * 45}deg)`,
                transformOrigin: '230px 190px',
              }}
              markerEnd="url(#clock-ah)"
            />

            {/* Center Pivot Hub */}
            <circle cx="230" cy="190" r="32" fill="var(--surface)" stroke="var(--border)" strokeWidth="2" />
            <text x="230" y="186" textAnchor="middle" fontSize="9" fontWeight="700" fill="var(--accent)" letterSpacing="0.05em">
              HAND
            </text>
            <text x="230" y="201" textAnchor="middle" fontSize="13" fontWeight="800" fontFamily="var(--font-num)" fill="var(--text)">
              F{hand}
            </text>

            {/* 8 Circular Frame Nodes */}
            {CLOCK_COORDS.map((coord, i) => {
              const frame = frames[i];
              const isPointer = hand === i;
              const isVictim = victimFrame === i;
              const isHit = hitFrame === i;
              const isPinned = frame.pinCount > 0;

              let nodeClass = 'clock-node';
              if (isVictim) nodeClass += ' victim';
              else if (isHit) nodeClass += ' hit';
              else if (isPointer) nodeClass += ' active-hand';
              else if (isPinned) nodeClass += ' pinned';

              return (
                <g key={i} className={nodeClass} transform={`translate(${coord.cx}, ${coord.cy})`}>
                  <circle cx="0" cy="0" r="25" />
                  <text x="0" y="-3" fontSize="11" fontWeight="700" fontFamily="var(--font-num)">
                    F{i}
                  </text>
                  <text x="0" y="11" fontSize="9" fontWeight="600" fill={frame.pageId !== null ? 'var(--text)' : 'var(--text-faint)'}>
                    {frame.pageId !== null ? `P-${frame.pageId}` : 'Empty'}
                  </text>

                  {/* Ref Bit Pill */}
                  <rect
                    x="-18"
                    y="-36"
                    width="36"
                    height="13"
                    rx="3"
                    fill={frame.refBit === 1 ? 'rgba(230, 160, 30, 0.2)' : 'var(--surface-2)'}
                    stroke={frame.refBit === 1 ? 'var(--accent)' : 'var(--border)'}
                    strokeWidth="0.8"
                  />
                  <text x="0" y="-27" textAnchor="middle" fontSize="8" fontWeight="700" fill={frame.refBit === 1 ? 'var(--accent)' : 'var(--text-muted)'}>
                    Ref:{frame.refBit}
                  </text>

                  {/* Dirty / Clean Tag */}
                  <rect
                    x="-18"
                    y="25"
                    width="36"
                    height="12"
                    rx="3"
                    fill={frame.dirty ? 'rgba(245, 158, 11, 0.2)' : 'rgba(16, 185, 129, 0.15)'}
                    stroke={frame.dirty ? '#f59e0b' : 'var(--success)'}
                    strokeWidth="0.8"
                  />
                  <text x="0" y="34" textAnchor="middle" fontSize="8" fontWeight="700" fill={frame.dirty ? '#f59e0b' : 'var(--success)'}>
                    {frame.dirty ? 'DIRTY' : 'CLEAN'}
                  </text>
                </g>
              );
            })}
          </svg>
        </div>

        {/* RIGHT: Status Log, Live Frame Table & Theory */}
        <div style={{ display: 'flex', flexDirection: 'column', gap: 14 }}>
          {/* Active Hand Inspector Card */}
          <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 16 }}>
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 6 }}>
              <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--accent)', textTransform: 'uppercase', letterSpacing: 0.5 }}>
                Current Inspection: Frame F{hand}
              </span>
              <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>
                {currentFrame.pinCount > 0 ? '📌 Pinned' : 'Unpinned'}
              </span>
            </div>
            <div style={{ fontSize: 14, fontWeight: 700, color: 'var(--text)', marginBottom: 6 }}>
              {currentFrame.pageId !== null ? `Holding Page P-${currentFrame.pageId}` : 'Empty Frame (Ready for Allocation)'}
            </div>
            <div
              style={{
                padding: '8px 12px',
                borderRadius: 6,
                fontSize: 12,
                fontWeight: 600,
                background:
                  victimFrame !== null
                    ? 'var(--danger-wash)'
                    : hitFrame !== null
                    ? 'rgba(16, 185, 129, 0.1)'
                    : 'var(--surface-2)',
                color:
                  victimFrame !== null
                    ? 'var(--danger)'
                    : hitFrame !== null
                    ? 'var(--success)'
                    : 'var(--accent)',
                borderLeft: `3px solid ${
                  victimFrame !== null
                    ? 'var(--danger)'
                    : hitFrame !== null
                    ? 'var(--success)'
                    : 'var(--accent)'
                }`,
              }}
            >
              {logMsg}
            </div>
          </div>

          {/* Live Frame Table */}
          <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 14 }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 8 }}>
              <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: 0.5 }}>
                Buffer Pool Frame Directory (8 Frames)
              </span>
              <span style={{ fontSize: 10, color: 'var(--text-faint)' }}>4KB Pages</span>
            </div>
            <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 11 }}>
              <thead>
                <tr style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-muted)', textAlign: 'left' }}>
                  <th style={{ padding: '4px 6px' }}>Frame</th>
                  <th style={{ padding: '4px 6px' }}>Page ID</th>
                  <th style={{ padding: '4px 6px' }}>Ref Bit</th>
                  <th style={{ padding: '4px 6px' }}>Dirty</th>
                  <th style={{ padding: '4px 6px', textAlign: 'right' }}>Status</th>
                </tr>
              </thead>
              <tbody>
                {frames.map((f, i) => {
                  const isPointer = hand === i;
                  const isVictim = victimFrame === i;
                  const isHit = hitFrame === i;
                  return (
                    <tr
                      key={f.id}
                      style={{
                        borderBottom: '1px solid rgba(255,255,255,0.04)',
                        background: isVictim
                          ? 'rgba(239, 68, 68, 0.08)'
                          : isHit
                          ? 'rgba(16, 185, 129, 0.08)'
                          : isPointer
                          ? 'var(--accent-wash)'
                          : 'transparent',
                      }}
                    >
                      <td style={{ padding: '5px 6px', fontWeight: 700, fontFamily: 'var(--font-num)' }}>
                        F{f.id} {isPointer && '◀'}
                      </td>
                      <td style={{ padding: '5px 6px' }}>
                        {f.pageId !== null ? `Page ${f.pageId}` : '—'}
                      </td>
                      <td style={{ padding: '5px 6px' }}>
                        <span style={{ color: f.refBit === 1 ? 'var(--accent)' : 'var(--text-muted)', fontWeight: 700 }}>
                          {f.refBit}
                        </span>
                      </td>
                      <td style={{ padding: '5px 6px', color: f.dirty ? '#f59e0b' : 'var(--success)', fontWeight: 600 }}>
                        {f.dirty ? 'DIRTY' : 'CLEAN'}
                      </td>
                      <td style={{ padding: '5px 6px', textAlign: 'right', fontWeight: 600 }}>
                        {isVictim ? (
                          <span style={{ color: 'var(--danger)' }}>VICTIM</span>
                        ) : isHit ? (
                          <span style={{ color: 'var(--success)' }}>CACHE HIT</span>
                        ) : f.pinCount > 0 ? (
                          <span style={{ color: '#8b5cf6' }}>PINNED</span>
                        ) : (
                          <span style={{ color: 'var(--text-faint)' }}>READY</span>
                        )}
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        </div>
      </div>
    </div>
  );
}

// ──────────────────────────────────────────────────────────────────────────────
// 2. LIVE LEDGER COMPONENT (High Legibility & Operation Filter)
// ──────────────────────────────────────────────────────────────────────────────

function LiveLedgerTable({ ledger }: { ledger: LedgerEntry[] }) {
  const [filter, setFilter] = useState<'ALL' | 'BID' | 'CREATE_ITEM' | 'CLOSE_ITEM'>('ALL');
  const [selectedEntry, setSelectedEntry] = useState<LedgerEntry | null>(null);
  const [searchTerm, setSearchTerm] = useState('');

  const filtered = useMemo(() => {
    return ledger.filter((e) => {
      const matchOp = filter === 'ALL' || e.op === filter;
      const matchSearch =
        searchTerm === '' ||
        e.txId.toLowerCase().includes(searchTerm.toLowerCase()) ||
        e.lsn.toLowerCase().includes(searchTerm.toLowerCase()) ||
        e.op.toLowerCase().includes(searchTerm.toLowerCase());
      return matchOp && matchSearch;
    });
  }, [ledger, filter, searchTerm]);

  return (
    <div className="card" style={{ padding: 24, marginBottom: 28 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', flexWrap: 'wrap', gap: 14, marginBottom: 16 }}>
        <div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
            <h2 className="section-title" style={{ margin: 0, fontSize: 17 }}>
              Live Ledger · Append-Only Write-Ahead Log (WAL) Trail
            </h2>
            <span
              style={{
                fontSize: 10,
                fontWeight: 700,
                padding: '3px 9px',
                borderRadius: 12,
                background: 'rgba(56, 189, 248, 0.12)',
                color: '#38bdf8',
                border: '1px solid #38bdf8',
                textTransform: 'uppercase',
                letterSpacing: 0.5,
              }}
            >
              REAL-TIME SSE STREAM
            </span>
          </div>
          <p style={{ fontSize: 12, color: 'var(--text-muted)', margin: '4px 0 0 0' }}>
            Chronological sequence of atomic, durable transactions flushed with O_DIRECT and fdatasync. Click any log frame to inspect its binary ARIES record decomposition.
          </p>
        </div>

        {/* Filter buttons & Search */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap' }}>
          <input
            type="text"
            placeholder="Search LSN / Tx ID..."
            value={searchTerm}
            onChange={(e) => setSearchTerm(e.target.value)}
            className="form-input"
            style={{ width: 170, fontSize: 12, padding: '5px 10px', height: 32 }}
          />
          <div style={{ display: 'flex', gap: 4, background: 'var(--surface-2)', padding: 3, borderRadius: 6 }}>
            {(['ALL', 'BID', 'CREATE_ITEM', 'CLOSE_ITEM'] as const).map((t) => (
              <button
                key={t}
                className={`btn btn-sm ${filter === t ? 'btn-primary' : 'btn-ghost'}`}
                onClick={() => setFilter(t)}
                style={{ fontSize: 11, padding: '4px 10px', height: 26 }}
              >
                {t === 'ALL' ? `All (${ledger.length})` : t}
              </button>
            ))}
          </div>
        </div>
      </div>

      {/* WAL Engine Metrics Header */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(130px, 1fr))', gap: 12, marginBottom: 18 }}>
        <div style={{ background: 'var(--surface-2)', padding: '10px 14px', borderRadius: 8, border: '1px solid var(--border)' }}>
          <div style={{ fontSize: 11, color: 'var(--text-muted)', textTransform: 'uppercase', fontFamily: 'var(--font-num)' }}>Committed LSNs</div>
          <div style={{ fontSize: 20, fontWeight: 700, color: 'var(--accent)', marginTop: 2, fontFamily: 'var(--font-num)' }}>{ledger.length}</div>
          <div style={{ fontSize: 11, color: 'var(--text-faint)', marginTop: 2 }}>Append-only log</div>
        </div>
        <div style={{ background: 'var(--surface-2)', padding: '10px 14px', borderRadius: 8, border: '1px solid var(--border)' }}>
          <div style={{ fontSize: 11, color: 'var(--text-muted)', textTransform: 'uppercase', fontFamily: 'var(--font-num)' }}>Disk Flush Mode</div>
          <div style={{ fontSize: 14, fontWeight: 700, color: 'var(--success)', marginTop: 4, fontFamily: 'var(--font-num)' }}>fdatasync()</div>
          <div style={{ fontSize: 11, color: 'var(--text-faint)', marginTop: 2 }}>Zero write cache lag</div>
        </div>
        <div style={{ background: 'var(--surface-2)', padding: '10px 14px', borderRadius: 8, border: '1px solid var(--border)' }}>
          <div style={{ fontSize: 11, color: 'var(--text-muted)', textTransform: 'uppercase', fontFamily: 'var(--font-num)' }}>Concurrency Mode</div>
          <div style={{ fontSize: 14, fontWeight: 700, color: '#38bdf8', marginTop: 4, fontFamily: 'var(--font-num)' }}>Strict 2PL</div>
          <div style={{ fontSize: 11, color: 'var(--text-faint)', marginTop: 2 }}>Hold locks to commit</div>
        </div>
        <div style={{ background: 'var(--surface-2)', padding: '10px 14px', borderRadius: 8, border: '1px solid var(--border)' }}>
          <div style={{ fontSize: 11, color: 'var(--text-muted)', textTransform: 'uppercase', fontFamily: 'var(--font-num)' }}>Integrity</div>
          <div style={{ fontSize: 14, fontWeight: 700, color: 'var(--success)', marginTop: 4, fontFamily: 'var(--font-num)' }}>CRC32 Verified</div>
          <div style={{ fontSize: 11, color: 'var(--text-faint)', marginTop: 2 }}>Slotted Page CRC-32C</div>
        </div>
      </div>

      {/* Selected Log Record Binary Inspector Drawer */}
      {selectedEntry && (
        <div style={{ background: 'rgba(230, 160, 30, 0.05)', border: '1px solid var(--accent)', borderRadius: 8, padding: 16, marginBottom: 18 }}>
          <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 10 }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
              <span style={{ fontSize: 12, fontWeight: 700, color: 'var(--accent)', textTransform: 'uppercase', letterSpacing: 0.5 }}>
                Binary WAL Record Frame Inspector · {selectedEntry.lsn}
              </span>
              <span className="badge" style={{ fontSize: 10, background: 'var(--accent-wash)', color: 'var(--accent)' }}>
                {selectedEntry.op}
              </span>
            </div>
            <button
              className="btn btn-ghost btn-sm"
              onClick={() => setSelectedEntry(null)}
              style={{ fontSize: 11, padding: '2px 8px' }}
            >
              ✕ Close Inspector
            </button>
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(200px, 1fr))', gap: 12, fontSize: 12 }}>
            <div style={{ background: 'var(--surface)', padding: 10, borderRadius: 6, border: '1px solid var(--border)' }}>
              <div style={{ color: 'var(--text-muted)', fontSize: 10, textTransform: 'uppercase' }}>Log Sequence Number</div>
              <div style={{ fontFamily: 'var(--font-num)', fontWeight: 700, color: 'var(--accent)', marginTop: 2 }}>{selectedEntry.lsn}</div>
            </div>
            <div style={{ background: 'var(--surface)', padding: 10, borderRadius: 6, border: '1px solid var(--border)' }}>
              <div style={{ color: 'var(--text-muted)', fontSize: 10, textTransform: 'uppercase' }}>Previous LSN (Undo Backlink)</div>
              <div style={{ fontFamily: 'var(--font-num)', fontWeight: 700, color: 'var(--text)', marginTop: 2 }}>00000001/00000000 (Start)</div>
            </div>
            <div style={{ background: 'var(--surface)', padding: 10, borderRadius: 6, border: '1px solid var(--border)' }}>
              <div style={{ color: 'var(--text-muted)', fontSize: 10, textTransform: 'uppercase' }}>Transaction ID</div>
              <div style={{ fontFamily: 'var(--font-num)', fontWeight: 700, color: 'var(--text)', marginTop: 2 }}>{selectedEntry.txId}</div>
            </div>
            <div style={{ background: 'var(--surface)', padding: 10, borderRadius: 6, border: '1px solid var(--border)' }}>
              <div style={{ color: 'var(--text-muted)', fontSize: 10, textTransform: 'uppercase' }}>Slotted Disk Page</div>
              <div style={{ fontFamily: 'var(--font-num)', fontWeight: 700, color: 'var(--success)', marginTop: 2 }}>Page P-1 · Slot #2 (Offset 0x0180)</div>
            </div>
          </div>
          <div style={{ marginTop: 10, fontSize: 11, color: 'var(--text-muted)' }}>
            <strong>Payload:</strong> Write-Ahead Logging rule adhered: This log record was synchronously synced to persistent storage before the dirty page frame was marked clean in volatile RAM.
          </div>
        </div>
      )}

      {/* Ledger Table */}
      {filtered.length === 0 ? (
        <div style={{ padding: '24px 16px', textAlign: 'center', color: 'var(--text-muted)', fontSize: 13, background: 'var(--surface)', borderRadius: 8, border: '1px solid var(--border)' }}>
          No entries found matching this filter. Real-time bids and lot creations will stream here automatically.
        </div>
      ) : (
        <div style={{ overflowX: 'auto', border: '1px solid var(--border)', borderRadius: 8 }}>
          <table className="lots-table" style={{ width: '100%', fontSize: 13 }}>
            <thead>
              <tr style={{ background: 'var(--surface-2)' }}>
                <th style={{ color: 'var(--text)', fontWeight: 700 }}>LSN (Log Sequence No.)</th>
                <th style={{ color: 'var(--text)', fontWeight: 700 }}>Transaction ID</th>
                <th style={{ color: 'var(--text)', fontWeight: 700 }}>Operation</th>
                <th style={{ color: 'var(--text)', fontWeight: 700 }}>Engine Status</th>
                <th style={{ color: 'var(--text)', fontWeight: 700, textAlign: 'right' }}>Committed At</th>
              </tr>
            </thead>
            <tbody>
              {filtered.map((e, idx) => {
                const lsnShort = e.lsn.includes('/') ? e.lsn.split('/')[1] : e.lsn;
                const isBid = e.op === 'BID';
                const isCreate = e.op === 'CREATE_ITEM';
                const isCommitted = e.status === 'COMMITTED';
                const isSelected = selectedEntry?.lsn === e.lsn;

                return (
                  <tr
                    key={`${e.lsn}-${idx}`}
                    onClick={() => setSelectedEntry(isSelected ? null : e)}
                    style={{
                      borderBottom: '1px solid var(--border)',
                      cursor: 'pointer',
                      background: isSelected ? 'var(--accent-wash)' : 'transparent',
                      transition: 'background 150ms ease',
                    }}
                    title="Click to inspect binary WAL record"
                  >
                    <td>
                      <span
                        style={{
                          fontFamily: 'var(--font-num)',
                          fontWeight: 700,
                          fontSize: 12,
                          color: 'var(--accent)',
                          background: 'var(--accent-wash)',
                          padding: '2px 8px',
                          borderRadius: 4,
                          border: '1px solid rgba(230, 160, 30, 0.25)',
                        }}
                      >
                        LSN #{lsnShort}
                      </span>
                    </td>
                    <td style={{ fontFamily: 'var(--font-num)', fontSize: 13, fontWeight: 600, color: 'var(--text)' }}>
                      {e.txId}
                    </td>
                    <td>
                      <span
                        className="badge"
                        style={
                          isBid
                            ? { background: 'rgba(230, 160, 30, 0.15)', color: 'var(--accent)', borderColor: 'var(--accent)' }
                            : isCreate
                            ? { background: 'rgba(56, 189, 248, 0.15)', color: '#38bdf8', borderColor: '#38bdf8' }
                            : { background: 'rgba(168, 85, 247, 0.15)', color: '#a855f7', borderColor: '#a855f7' }
                        }
                      >
                        {e.op}
                      </span>
                    </td>
                    <td>
                      <span
                        className="badge"
                        style={{
                          background: isCommitted ? 'rgba(16, 185, 129, 0.15)' : 'rgba(239, 68, 68, 0.15)',
                          color: isCommitted ? 'var(--success)' : 'var(--danger)',
                          borderColor: isCommitted ? 'var(--success)' : 'var(--danger)',
                          display: 'inline-flex',
                          alignItems: 'center',
                          gap: 5,
                        }}
                      >
                        <span style={{ width: 6, height: 6, borderRadius: '50%', background: isCommitted ? 'var(--success)' : 'var(--danger)' }} />
                        {e.status}
                      </span>
                    </td>
                    <td style={{ textAlign: 'right', fontFamily: 'var(--font-num)', fontSize: 12, color: 'var(--text-muted)' }}>
                      {new Date(e.at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}

// ──────────────────────────────────────────────────────────────────────────────
// 3. STRICT 2PL BID JOURNEY VISUALIZER (Interactive Stepper & Phase Meter)
// ──────────────────────────────────────────────────────────────────────────────

interface TwoPLStepData {
  step: number;
  tag: string;
  tagColor: string;
  tagBg: string;
  title: string;
  phase: 'growing' | 'lock_point' | 'shrinking' | 'broadcast';
  phaseLabel: string;
  phaseColor: string;
  activeBox: 'http' | 'lock_mgr' | 'rules' | 'wal' | 'buffer' | 'sse';
  timeMs: number;
  description: string;
  deepDive: string;
  telemetry: {
    resource: string;
    lockMode: string;
    lockStatus: string;
    assignedLsn: string;
    bufferFrame: string;
    queueWaiters: string;
    networkLatency: string;
  };
  lockTable: Array<{
    txId: string;
    client: string;
    resource: string;
    mode: string;
    phase: string;
    status: string;
    statusColor: string;
  }>;
}

const TWO_PL_STEPS: TwoPLStepData[] = [
  {
    step: 0,
    tag: 'HTTP / INGRESS',
    tagColor: '#38bdf8',
    tagBg: 'rgba(56, 189, 248, 0.12)',
    title: 'Client Bid Ingress & Payload Validation',
    phase: 'growing',
    phaseLabel: 'GROWING PHASE (INITIALIZING)',
    phaseColor: '#3b82f6',
    activeBox: 'http',
    timeMs: 0.0,
    description: 'Bidder Kanak (Tx-104) submits an HTTP POST /api/bid request offering $1,450 for Lot #2 (1968 Rolex Submariner).',
    deepDive: 'The web worker parses incoming JSON, verifies the user authentication token, and validates that Lot #2 is currently ACTIVE. No database locks are acquired yet, so concurrent transactions run unhindered.',
    telemetry: {
      resource: 'Lot #2 (Rolex Submariner)',
      lockMode: 'None (Unreserved)',
      lockStatus: 'REQUEST_PENDING',
      assignedLsn: 'None',
      bufferFrame: 'None',
      queueWaiters: '0 Waiters',
      networkLatency: '0.0 ms',
    },
    lockTable: [
      { txId: 'Tx-104', client: 'Kanak', resource: 'Lot #2', mode: 'PENDING', phase: 'Growing', status: 'ACQUIRING...', statusColor: '#f59e0b' },
      { txId: 'Tx-105', client: 'Vinayak', resource: 'Lot #2', mode: 'QUEUED', phase: 'Waiting', status: 'IN QUEUE', statusColor: 'var(--text-muted)' },
      { txId: 'Tx-101', client: 'Admin', resource: 'Lot #1', mode: 'SHARED (S)', phase: 'Active', status: 'HELD', statusColor: 'var(--success)' },
    ],
  },
  {
    step: 1,
    tag: '2PL / LOCK-ACQUIRE',
    tagColor: '#f59e0b',
    tagBg: 'rgba(245, 158, 11, 0.12)',
    title: 'Exclusive Lock Acquisition (Strict 2PL Growing Phase)',
    phase: 'growing',
    phaseLabel: 'GROWING PHASE (LOCK ACQUIRED)',
    phaseColor: '#f59e0b',
    activeBox: 'lock_mgr',
    timeMs: 0.4,
    description: 'The Lock Manager grants Tx-104 an EXCLUSIVE (X-LOCK) on Resource "Lot:2". Concurrent bidder Tx-105 is blocked in the lock queue.',
    deepDive: 'Under Strict Two-Phase Locking, a transaction may acquire locks during its growing phase. An exclusive write lock (X) prevents all concurrent transactions from reading or modifying Lot #2, guaranteeing conflict serializability. Concurrently arriving bid from Tx-105 (Vinayak) is queued until Tx-104 commits.',
    telemetry: {
      resource: 'Lot #2 (Rolex Submariner)',
      lockMode: 'EXCLUSIVE (X-LOCK)',
      lockStatus: 'GRANTED (GROWING)',
      assignedLsn: 'Pending WAL',
      bufferFrame: 'Frame #3 (Reserved)',
      queueWaiters: '1 Blocked (Tx-105)',
      networkLatency: '+0.4 ms',
    },
    lockTable: [
      { txId: 'Tx-104', client: 'Kanak', resource: 'Lot #2', mode: 'EXCLUSIVE (X)', phase: 'Growing', status: 'HELD (EXCLUSIVE)', statusColor: 'var(--accent)' },
      { txId: 'Tx-105', client: 'Vinayak', resource: 'Lot #2', mode: 'EXCLUSIVE (X)', phase: 'Blocked', status: 'BLOCKED WAITING', statusColor: 'var(--danger)' },
      { txId: 'Tx-101', client: 'Admin', resource: 'Lot #1', mode: 'SHARED (S)', phase: 'Active', status: 'HELD', statusColor: 'var(--success)' },
    ],
  },
  {
    step: 2,
    tag: 'RULES / ANTI-SNIPING',
    tagColor: '#a855f7',
    tagBg: 'rgba(168, 85, 247, 0.12)',
    title: 'Auction Rules & Soft-Close Anti-Sniping Verification',
    phase: 'growing',
    phaseLabel: 'GROWING PHASE (VALIDATION)',
    phaseColor: '#f59e0b',
    activeBox: 'rules',
    timeMs: 0.6,
    description: 'Because Tx-104 holds the exclusive lock, it safely reads current high bid ($1,400) without race conditions. Bid $1,450 satisfies the +$50 minimum increment.',
    deepDive: 'The auction engine checks if the bid arrived within the final 30 seconds of the auction lot timer. Because anti-sniping protection is triggered, the closing deadline is automatically extended by +60 seconds, giving rival bidders a fair window to respond while holding the lot locked.',
    telemetry: {
      resource: 'Lot #2 (Rolex Submariner)',
      lockMode: 'EXCLUSIVE (X-LOCK)',
      lockStatus: 'HELD (PROTECTED)',
      assignedLsn: 'Pre-Alloc 0x0042',
      bufferFrame: 'Frame #3',
      queueWaiters: '1 Blocked (Tx-105)',
      networkLatency: '+0.6 ms',
    },
    lockTable: [
      { txId: 'Tx-104', client: 'Kanak', resource: 'Lot #2', mode: 'EXCLUSIVE (X)', phase: 'Growing', status: 'HELD (EXCLUSIVE)', statusColor: 'var(--accent)' },
      { txId: 'Tx-105', client: 'Vinayak', resource: 'Lot #2', mode: 'EXCLUSIVE (X)', phase: 'Blocked', status: 'BLOCKED WAITING', statusColor: 'var(--danger)' },
      { txId: 'Tx-101', client: 'Admin', resource: 'Lot #1', mode: 'SHARED (S)', phase: 'Active', status: 'HELD', statusColor: 'var(--success)' },
    ],
  },
  {
    step: 3,
    tag: 'WAL / FDATASYNC',
    tagColor: '#38bdf8',
    tagBg: 'rgba(56, 189, 248, 0.12)',
    title: 'Write-Ahead Log (WAL) Flushed to Non-Volatile Disk',
    phase: 'growing',
    phaseLabel: 'GROWING PHASE (WAL FLUSH)',
    phaseColor: '#38bdf8',
    activeBox: 'wal',
    timeMs: 1.1,
    description: 'Append-only WAL record is serialized to the log buffer and flushed to physical disk via fdatasync() before any in-memory pages are touched.',
    deepDive: 'Fundamental WAL Rule: The log record describing a change must be flushed to stable storage before the dirty page can be written to disk. LSN 0x00000042 is assigned with TxID 104, PageID 1, Slot 4, Undo: $1,400, Redo: $1,450, and CRC32 checksum. This guarantees the Durability (D in ACID) requirement.',
    telemetry: {
      resource: 'Lot #2 (Rolex Submariner)',
      lockMode: 'EXCLUSIVE (X-LOCK)',
      lockStatus: 'HELD (FLUSHING)',
      assignedLsn: '0x00000042 (Synced)',
      bufferFrame: 'Frame #3 (Pre-dirty)',
      queueWaiters: '1 Blocked (Tx-105)',
      networkLatency: '+1.1 ms',
    },
    lockTable: [
      { txId: 'Tx-104', client: 'Kanak', resource: 'Lot #2', mode: 'EXCLUSIVE (X)', phase: 'Growing', status: 'HELD (EXCLUSIVE)', statusColor: 'var(--accent)' },
      { txId: 'Tx-105', client: 'Vinayak', resource: 'Lot #2', mode: 'EXCLUSIVE (X)', phase: 'Blocked', status: 'BLOCKED WAITING', statusColor: 'var(--danger)' },
      { txId: 'Tx-101', client: 'Admin', resource: 'Lot #1', mode: 'SHARED (S)', phase: 'Active', status: 'HELD', statusColor: 'var(--success)' },
    ],
  },
  {
    step: 4,
    tag: 'RAM / SLOTTED-PAGE',
    tagColor: 'var(--accent)',
    tagBg: 'var(--accent-wash)',
    title: 'Slotted Page in Buffer Pool Updated in Memory',
    phase: 'lock_point',
    phaseLabel: 'LOCK POINT (PEAK GROWING PHASE)',
    phaseColor: 'var(--accent)',
    activeBox: 'buffer',
    timeMs: 1.5,
    description: 'Buffer Manager locates Page P-1 in Frame #3, updates the slotted record with the new bid ($1,450), sets PageLSN=0x00000042, and marks the page DIRTY.',
    deepDive: 'This step represents the Strict 2PL "Lock Point": all locks needed have been acquired and all state mutations are complete in memory. Clock replacement pins Frame #3 to prevent premature eviction until the transaction finishes. No locks have been released yet.',
    telemetry: {
      resource: 'Lot #2 (Rolex Submariner)',
      lockMode: 'EXCLUSIVE (X-LOCK)',
      lockStatus: 'HELD (LOCK POINT)',
      assignedLsn: '0x00000042',
      bufferFrame: 'Frame #3 (DIRTY, Pinned)',
      queueWaiters: '1 Blocked (Tx-105)',
      networkLatency: '+1.5 ms',
    },
    lockTable: [
      { txId: 'Tx-104', client: 'Kanak', resource: 'Lot #2', mode: 'EXCLUSIVE (X)', phase: 'Lock Point', status: 'MAX LOCKS HELD', statusColor: 'var(--accent)' },
      { txId: 'Tx-105', client: 'Vinayak', resource: 'Lot #2', mode: 'EXCLUSIVE (X)', phase: 'Blocked', status: 'BLOCKED WAITING', statusColor: 'var(--danger)' },
      { txId: 'Tx-101', client: 'Admin', resource: 'Lot #1', mode: 'SHARED (S)', phase: 'Active', status: 'HELD', statusColor: 'var(--success)' },
    ],
  },
  {
    step: 5,
    tag: '2PL / STRICT-RELEASE',
    tagColor: 'var(--success)',
    tagBg: 'rgba(16, 185, 129, 0.12)',
    title: 'Transaction Commit & Simultaneous Strict Lock Release',
    phase: 'shrinking',
    phaseLabel: 'SHRINKING PHASE (STRICT RELEASE)',
    phaseColor: 'var(--success)',
    activeBox: 'lock_mgr',
    timeMs: 1.9,
    description: 'Flushes COMMIT WAL record (LSN 0x00000043). Under Strict 2PL, ALL exclusive locks are released simultaneously upon commit, instantly unblocking Tx-105!',
    deepDive: 'Strict Two-Phase Locking Theorem: Unlike standard 2PL where locks can be gradually released in the shrinking phase, Strict 2PL retains all exclusive write locks until the final COMMIT or ABORT record is flushed. This mathematically guarantees Conflict Serializability AND Strict Recoverability with Zero Cascading Aborts—no concurrent transaction ever witnesses dirty or uncommitted state.',
    telemetry: {
      resource: 'Lot #2 (Rolex Submariner)',
      lockMode: 'NONE (RELEASED)',
      lockStatus: 'COMMITTED & RELEASED',
      assignedLsn: '0x00000043 (Commit)',
      bufferFrame: 'Frame #3 (Unpinned)',
      queueWaiters: '0 (Tx-105 Woken Up!)',
      networkLatency: '+1.9 ms',
    },
    lockTable: [
      { txId: 'Tx-104', client: 'Kanak', resource: 'Lot #2', mode: 'RELEASED', phase: 'Committed', status: 'ALL LOCKS FREED', statusColor: 'var(--success)' },
      { txId: 'Tx-105', client: 'Vinayak', resource: 'Lot #2', mode: 'EXCLUSIVE (X)', phase: 'Growing', status: 'ACQUIRED LOCK!', statusColor: '#38bdf8' },
      { txId: 'Tx-101', client: 'Admin', resource: 'Lot #1', mode: 'SHARED (S)', phase: 'Active', status: 'HELD', statusColor: 'var(--success)' },
    ],
  },
  {
    step: 6,
    tag: 'SSE / BROADCAST',
    tagColor: 'var(--success)',
    tagBg: 'rgba(16, 185, 129, 0.12)',
    title: 'Real-Time SSE Event Broadcasted to All Bidders',
    phase: 'broadcast',
    phaseLabel: 'DURABLE BROADCAST COMPLETE',
    phaseColor: 'var(--success)',
    activeBox: 'sse',
    timeMs: 2.4,
    description: 'Event loop dispatches an SSE event payload with the new high bid to all connected browser clients. Live Auction Floor and Host Panel update in under 1 ms.',
    deepDive: 'Because WAL and Buffer Pool state is strictly committed and durable, the broadcast is 100% authoritative. Clients consume the event via EventSource("/api/stream") to update live bid meters, highest bidder badge, and the extended countdown clock simultaneously.',
    telemetry: {
      resource: 'Lot #2 (Rolex Submariner)',
      lockMode: 'FREE',
      lockStatus: 'TRANSACTION_FINAL',
      assignedLsn: '0x00000043',
      bufferFrame: 'Frame #3',
      queueWaiters: '0 (Clean State)',
      networkLatency: '+2.4 ms (Total E2E)',
    },
    lockTable: [
      { txId: 'Tx-104', client: 'Kanak', resource: 'Lot #2', mode: 'RELEASED', phase: 'Done', status: 'TRANSACTION COMMITTED', statusColor: 'var(--success)' },
      { txId: 'Tx-105', client: 'Vinayak', resource: 'Lot #2', mode: 'EXCLUSIVE (X)', phase: 'Growing', status: 'PROCESSING BID', statusColor: '#38bdf8' },
      { txId: 'Tx-101', client: 'Admin', resource: 'Lot #1', mode: 'SHARED (S)', phase: 'Active', status: 'HELD', statusColor: 'var(--success)' },
    ],
  },
];

function Strict2PLJourneyVisualizer() {
  const [currentStep, setCurrentStep] = useState(0);
  const [isPlaying, setIsPlaying] = useState(false);

  const step = TWO_PL_STEPS[currentStep];

  useEffect(() => {
    if (!isPlaying) return;
    const timer = setInterval(() => {
      setCurrentStep((prev) => (prev + 1) % TWO_PL_STEPS.length);
    }, 2800);
    return () => clearInterval(timer);
  }, [isPlaying]);

  function prevStep() {
    setIsPlaying(false);
    setCurrentStep((prev) => (prev > 0 ? prev - 1 : TWO_PL_STEPS.length - 1));
  }

  function nextStep() {
    setIsPlaying(false);
    setCurrentStep((prev) => (prev + 1) % TWO_PL_STEPS.length);
  }

  function resetJourney() {
    setIsPlaying(false);
    setCurrentStep(0);
  }

  return (
    <div className="card" style={{ padding: 24, marginBottom: 28 }}>
      {/* Header with Title and Interactive Controls */}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', flexWrap: 'wrap', gap: 14, marginBottom: 16 }}>
        <div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 4 }}>
            <span className="badge" style={{ background: 'var(--accent-wash)', color: 'var(--accent)', fontSize: 10, fontWeight: 700 }}>
              CONCURRENCY CONTROL ENGINE
            </span>
            <span style={{ fontSize: 11, color: 'var(--text-faint)', fontFamily: 'var(--font-num)' }}>
              RIGOROUS ACID GUARANTEES
            </span>
          </div>
          <h2 className="section-title" style={{ margin: 0, fontSize: 18 }}>
            Strict 2PL Bid Journey (Two-Phase Locking)
          </h2>
          <p style={{ fontSize: 12, color: 'var(--text-muted)', margin: '4px 0 0 0', maxWidth: 660 }}>
            Every incoming bid executes inside an atomic ACID transaction. Strict 2PL acquires exclusive locks during the Growing Phase and holds them strictly until commit, preventing dirty reads, unrepeatable reads, and cascading aborts.
          </p>
        </div>

        {/* Stepper Controls */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap' }}>
          <button className="btn btn-ghost btn-sm" onClick={resetJourney} title="Reset to Step 1" style={{ fontSize: 12 }}>
            ↺ Reset
          </button>
          <button className="btn btn-ghost btn-sm" onClick={prevStep} title="Previous Step" style={{ fontSize: 12 }}>
            ◀ Prev
          </button>
          <button
            className={`btn btn-sm ${isPlaying ? 'btn-ghost' : 'btn-primary'}`}
            onClick={() => setIsPlaying(!isPlaying)}
            style={{ fontSize: 12, minWidth: 105 }}
          >
            {isPlaying ? '⏸ Pause' : '▶ Auto-Play'}
          </button>
          <button className="btn btn-ghost btn-sm" onClick={nextStep} title="Next Step" style={{ fontSize: 12 }}>
            Next ▶
          </button>
        </div>
      </div>

      {/* Stepper Navigation Dots */}
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', flexWrap: 'wrap', gap: 6, marginBottom: 16 }}>
        <div style={{ display: 'flex', gap: 6, alignItems: 'center', flexWrap: 'wrap' }}>
          {TWO_PL_STEPS.map((s, idx) => (
            <button
              key={s.step}
              onClick={() => { setIsPlaying(false); setCurrentStep(idx); }}
              style={{
                display: 'flex',
                alignItems: 'center',
                gap: 6,
                padding: '4px 10px',
                borderRadius: 16,
                border: `1px solid ${currentStep === idx ? s.tagColor : 'var(--border)'}`,
                background: currentStep === idx ? s.tagBg : 'var(--surface-2)',
                color: currentStep === idx ? s.tagColor : 'var(--text-muted)',
                fontSize: 11,
                fontWeight: currentStep === idx ? 700 : 500,
                cursor: 'pointer',
                transition: 'all 200ms ease',
              }}
            >
              <span
                style={{
                  width: 16,
                  height: 16,
                  borderRadius: '50%',
                  display: 'inline-flex',
                  alignItems: 'center',
                  justifyContent: 'center',
                  background: currentStep === idx ? s.tagColor : 'rgba(255,255,255,0.08)',
                  color: currentStep === idx ? '#111' : 'var(--text-muted)',
                  fontSize: 10,
                  fontWeight: 700,
                }}
              >
                {idx + 1}
              </span>
              <span>{s.tag.split('/')[0].trim()}</span>
            </button>
          ))}
        </div>
        <div style={{ fontSize: 11, fontFamily: 'var(--font-num)', color: 'var(--text-muted)' }}>
          Step {currentStep + 1} of {TWO_PL_STEPS.length}
        </div>
      </div>

      {/* Strict 2PL Phase Meter */}
      <div style={{ marginBottom: 18 }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 6, fontSize: 11 }}>
          <span style={{ fontWeight: 700, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: 0.5 }}>
            Two-Phase Locking Protocol Phase Indicator
          </span>
          <span style={{ fontWeight: 700, color: step.phaseColor, fontFamily: 'var(--font-num)' }}>
            ● {step.phaseLabel}
          </span>
        </div>
        <div className="two-pl-phase-meter">
          {/* Section 1: Growing Phase (Steps 0 - 3) */}
          <div
            className="two-pl-phase-part"
            style={{
              flex: 4,
              background: currentStep <= 3 ? 'rgba(245, 158, 11, 0.22)' : 'rgba(245, 158, 11, 0.08)',
              color: currentStep <= 3 ? '#f59e0b' : 'rgba(245, 158, 11, 0.5)',
              borderRight: '1px solid var(--border)',
            }}
          >
            1. Growing Phase (Acquire Only)
          </div>
          {/* Section 2: Lock Point (Step 4) */}
          <div
            className="two-pl-phase-part"
            style={{
              flex: 2,
              background: currentStep === 4 ? 'rgba(230, 160, 30, 0.35)' : 'rgba(230, 160, 30, 0.08)',
              color: currentStep === 4 ? 'var(--accent)' : 'rgba(230, 160, 30, 0.5)',
              borderRight: '1px solid var(--border)',
            }}
          >
            2. Lock Point (Peak)
          </div>
          {/* Section 3: Shrinking Phase & Release (Steps 5 - 6) */}
          <div
            className="two-pl-phase-part"
            style={{
              flex: 3,
              background: currentStep >= 5 ? 'rgba(16, 185, 129, 0.25)' : 'rgba(16, 185, 129, 0.08)',
              color: currentStep >= 5 ? 'var(--success)' : 'rgba(16, 185, 129, 0.5)',
            }}
          >
            3. Strict Shrink (Release on Commit)
          </div>
        </div>
      </div>

      {/* Interactive SVG Architecture Pipeline */}
      <div className="two-pl-svg-wrap" style={{ marginBottom: 20 }}>
        <svg viewBox="0 0 760 170" style={{ width: '100%', height: 'auto', display: 'block' }}>
          <defs>
            <marker id="two-pl-arrow" viewBox="0 0 10 10" refX="7" refY="5" markerWidth="6" markerHeight="6" orient="auto-start-reverse">
              <path d="M 0 1 L 9 5 L 0 9 z" fill="var(--border-strong)" />
            </marker>
            <marker id="two-pl-arrow-active" viewBox="0 0 10 10" refX="7" refY="5" markerWidth="6" markerHeight="6" orient="auto-start-reverse">
              <path d="M 0 1 L 9 5 L 0 9 z" fill="var(--accent)" />
            </marker>
            <filter id="two-pl-glow" x="-20%" y="-20%" width="140%" height="140%">
              <feGaussianBlur stdDeviation="3" result="blur" />
              <feComposite in="SourceGraphic" in2="blur" operator="over" />
            </filter>
          </defs>

          {/* Bus Grid lines / Background Wire Connections */}
          <line
            x1="115" y1="70" x2="145" y2="70"
            className={`two-pl-wire ${step.activeBox === 'lock_mgr' || currentStep >= 1 ? 'active' : ''}`}
            markerEnd={step.activeBox === 'lock_mgr' ? 'url(#two-pl-arrow-active)' : 'url(#two-pl-arrow)'}
          />
          <line
            x1="250" y1="70" x2="280" y2="70"
            className={`two-pl-wire ${step.activeBox === 'rules' || currentStep >= 2 ? 'active' : ''}`}
            markerEnd={step.activeBox === 'rules' ? 'url(#two-pl-arrow-active)' : 'url(#two-pl-arrow)'}
          />
          <line
            x1="390" y1="70" x2="420" y2="70"
            className={`two-pl-wire ${step.activeBox === 'wal' || currentStep >= 3 ? 'active' : ''}`}
            markerEnd={step.activeBox === 'wal' ? 'url(#two-pl-arrow-active)' : 'url(#two-pl-arrow)'}
          />
          <line
            x1="525" y1="70" x2="555" y2="70"
            className={`two-pl-wire ${step.activeBox === 'buffer' || currentStep >= 4 ? 'active' : ''}`}
            markerEnd={step.activeBox === 'buffer' ? 'url(#two-pl-arrow-active)' : 'url(#two-pl-arrow)'}
          />
          <line
            x1="650" y1="70" x2="675" y2="70"
            className={`two-pl-wire ${step.activeBox === 'sse' || currentStep >= 6 ? 'active' : ''}`}
            markerEnd={step.activeBox === 'sse' ? 'url(#two-pl-arrow-active)' : 'url(#two-pl-arrow)'}
          />

          {/* Commit feedback wire from Buffer Pool back to Lock Manager (Strict 2PL Release) */}
          <path
            d="M 602 105 C 602 145, 197 145, 197 105"
            fill="none"
            stroke={currentStep >= 5 ? 'var(--success)' : 'var(--border)'}
            strokeWidth={currentStep === 5 ? 2.5 : 1.5}
            strokeDasharray={currentStep === 5 ? '6 3' : '3 3'}
            className={currentStep === 5 ? 'two-pl-wire active' : ''}
          />
          <text x="400" y="148" textAnchor="middle" fontSize="10" fontWeight="600" fill={currentStep === 5 ? 'var(--success)' : 'var(--text-faint)'}>
            {currentStep === 5 ? '⚡ Commit Signal: Release All Held Locks Simultaneously' : 'Strict Release Loop on Commit'}
          </text>

          {/* BOX 1: HTTP INGRESS */}
          <g className={`two-pl-box ${step.activeBox === 'http' ? 'active' : ''}`}>
            <rect x="20" y="35" width="95" height="70" rx="8" fill="var(--surface)" stroke="var(--border)" strokeWidth="1.5" />
            <text x="67" y="58" textAnchor="middle" fontSize="11" fontWeight="700" fill={step.activeBox === 'http' ? 'var(--accent)' : 'var(--text)'}>
              1. HTTP API
            </text>
            <text x="67" y="74" textAnchor="middle" fontSize="9" fill="var(--text-muted)">
              POST /api/bid
            </text>
            <text x="67" y="90" textAnchor="middle" fontSize="10" fontWeight="600" fill="#38bdf8">
              Kanak: $1,450
            </text>
          </g>

          {/* BOX 2: LOCK MANAGER (STRICT 2PL) */}
          <g className={`two-pl-box ${step.activeBox === 'lock_mgr' ? 'active' : currentStep >= 1 && currentStep <= 4 ? 'locked' : currentStep >= 5 ? 'committed' : ''}`}>
            <rect x="145" y="35" width="105" height="70" rx="8" fill="var(--surface)" stroke="var(--border)" strokeWidth="1.5" />
            <text x="197" y="58" textAnchor="middle" fontSize="11" fontWeight="700" fill={currentStep >= 1 && currentStep <= 4 ? '#f59e0b' : currentStep >= 5 ? 'var(--success)' : 'var(--text)'}>
              2. Lock Mgr
            </text>
            <text x="197" y="74" textAnchor="middle" fontSize="9" fill="var(--text-muted)">
              Resource: Lot 2
            </text>
            <text x="197" y="90" textAnchor="middle" fontSize="10" fontWeight="700" fill={currentStep >= 1 && currentStep <= 4 ? '#f59e0b' : currentStep >= 5 ? 'var(--success)' : 'var(--text-faint)'}>
              {currentStep === 0 ? 'UNLOCKED' : currentStep <= 4 ? '🔒 X-LOCKED' : '🔓 RELEASED'}
            </text>
          </g>

          {/* BOX 3: AUCTION RULES ENGINE */}
          <g className={`two-pl-box ${step.activeBox === 'rules' ? 'active' : ''}`}>
            <rect x="280" y="35" width="110" height="70" rx="8" fill="var(--surface)" stroke="var(--border)" strokeWidth="1.5" />
            <text x="335" y="58" textAnchor="middle" fontSize="11" fontWeight="700" fill={step.activeBox === 'rules' ? 'var(--accent)' : 'var(--text)'}>
              3. Auction Rules
            </text>
            <text x="335" y="74" textAnchor="middle" fontSize="9" fill="var(--text-muted)">
              Min Incr: +$50 ✓
            </text>
            <text x="335" y="90" textAnchor="middle" fontSize="10" fontWeight="600" fill="#a855f7">
              {currentStep >= 2 ? 'Anti-Sniping +60s' : 'Soft-Close Check'}
            </text>
          </g>

          {/* BOX 4: WAL ENGINE (DISK FLUSH) */}
          <g className={`two-pl-box ${step.activeBox === 'wal' ? 'active' : ''}`}>
            <rect x="420" y="35" width="105" height="70" rx="8" fill="var(--surface)" stroke="var(--border)" strokeWidth="1.5" />
            <text x="472" y="58" textAnchor="middle" fontSize="11" fontWeight="700" fill={step.activeBox === 'wal' ? 'var(--accent)' : 'var(--text)'}>
              4. WAL Engine
            </text>
            <text x="472" y="74" textAnchor="middle" fontSize="9" fill="var(--text-muted)">
              fdatasync() disk
            </text>
            <text x="472" y="90" textAnchor="middle" fontSize="10" fontWeight="700" fill="#38bdf8">
              {currentStep >= 3 ? 'LSN: 0x0042' : 'Pending Sync'}
            </text>
          </g>

          {/* BOX 5: BUFFER POOL & SLOTTED PAGE */}
          <g className={`two-pl-box ${step.activeBox === 'buffer' ? 'active' : ''}`}>
            <rect x="555" y="35" width="95" height="70" rx="8" fill="var(--surface)" stroke="var(--border)" strokeWidth="1.5" />
            <text x="602" y="58" textAnchor="middle" fontSize="11" fontWeight="700" fill={step.activeBox === 'buffer' ? 'var(--accent)' : 'var(--text)'}>
              5. Buffer Pool
            </text>
            <text x="602" y="74" textAnchor="middle" fontSize="9" fill="var(--text-muted)">
              Frame #3 (RAM)
            </text>
            <text x="602" y="90" textAnchor="middle" fontSize="10" fontWeight="700" fill={currentStep >= 4 ? 'var(--accent)' : 'var(--text-faint)'}>
              {currentStep >= 4 ? 'DIRTY (P-1)' : 'Clean'}
            </text>
          </g>

          {/* BOX 6: SERVER-SENT EVENTS BROADCAST */}
          <g className={`two-pl-box ${step.activeBox === 'sse' ? 'active' : ''}`}>
            <rect x="675" y="35" width="75" height="70" rx="8" fill="var(--surface)" stroke="var(--border)" strokeWidth="1.5" />
            <text x="712" y="58" textAnchor="middle" fontSize="11" fontWeight="700" fill={step.activeBox === 'sse' ? 'var(--success)' : 'var(--text)'}>
              6. SSE
            </text>
            <text x="712" y="74" textAnchor="middle" fontSize="9" fill="var(--text-muted)">
              Stream Out
            </text>
            <text x="712" y="90" textAnchor="middle" fontSize="10" fontWeight="600" fill={currentStep >= 6 ? 'var(--success)' : 'var(--text-faint)'}>
              {currentStep >= 6 ? 'BROADCAST' : 'Idle'}
            </text>
          </g>

          {/* Moving token indicator representing current execution progress */}
          <circle
            cx={
              step.activeBox === 'http' ? 67 :
              step.activeBox === 'lock_mgr' ? 197 :
              step.activeBox === 'rules' ? 335 :
              step.activeBox === 'wal' ? 472 :
              step.activeBox === 'buffer' ? 602 : 712
            }
            cy="18"
            r="5"
            fill="var(--accent)"
            filter="url(#two-pl-glow)"
          />
          <text
            x={
              step.activeBox === 'http' ? 67 :
              step.activeBox === 'lock_mgr' ? 197 :
              step.activeBox === 'rules' ? 335 :
              step.activeBox === 'wal' ? 472 :
              step.activeBox === 'buffer' ? 602 : 712
            }
            y="9"
            textAnchor="middle"
            fontSize="8"
            fontWeight="700"
            fill="var(--accent)"
          >
            ACTIVE STAGE
          </text>
        </svg>
      </div>

      {/* Two-Column Deep-Dive State Cards */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(320px, 1fr))', gap: 18 }}>
        {/* Left: Step Details, Engine Explanation & Telemetry */}
        <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
          <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 16 }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 8 }}>
              <span
                style={{
                  fontSize: 10,
                  fontWeight: 700,
                  fontFamily: 'var(--font-num)',
                  textTransform: 'uppercase',
                  color: step.tagColor,
                  background: step.tagBg,
                  padding: '3px 8px',
                  borderRadius: 4,
                }}
              >
                {step.tag}
              </span>
              <span style={{ fontSize: 11, fontFamily: 'var(--font-num)', color: 'var(--text-muted)' }}>
                +{step.timeMs.toFixed(1)} ms elapsed
              </span>
            </div>
            <h3 style={{ margin: '0 0 6px 0', fontSize: 15, fontWeight: 700, color: 'var(--text)' }}>
              {step.title}
            </h3>
            <p style={{ margin: '0 0 10px 0', fontSize: 12, color: 'var(--text-muted)', lineHeight: 1.5 }}>
              {step.description}
            </p>
            <div
              style={{
                padding: '10px 12px',
                borderRadius: 6,
                background: 'var(--surface-2)',
                borderLeft: `3px solid ${step.tagColor}`,
                fontSize: 11,
                lineHeight: 1.5,
                color: 'var(--text-muted)',
              }}
            >
              <strong style={{ color: 'var(--text)' }}>Database Engine Principle: </strong>
              {step.deepDive}
            </div>
          </div>

          {/* Telemetry Grid */}
          <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 14 }}>
            <div style={{ fontSize: 11, fontWeight: 700, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: 0.5, marginBottom: 10 }}>
              Live ACID Transaction Telemetry
            </div>
            <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 8, fontSize: 11 }}>
              <div style={{ background: 'var(--surface-2)', padding: '6px 10px', borderRadius: 4 }}>
                <div style={{ fontSize: 10, color: 'var(--text-faint)' }}>TARGET RESOURCE</div>
                <div style={{ fontWeight: 600, color: 'var(--text)' }}>{step.telemetry.resource}</div>
              </div>
              <div style={{ background: 'var(--surface-2)', padding: '6px 10px', borderRadius: 4 }}>
                <div style={{ fontSize: 10, color: 'var(--text-faint)' }}>LOCK MODE</div>
                <div style={{ fontWeight: 600, color: step.telemetry.lockMode.includes('EXCLUSIVE') ? '#f59e0b' : step.telemetry.lockMode.includes('NONE') ? 'var(--success)' : 'var(--text)' }}>
                  {step.telemetry.lockMode}
                </div>
              </div>
              <div style={{ background: 'var(--surface-2)', padding: '6px 10px', borderRadius: 4 }}>
                <div style={{ fontSize: 10, color: 'var(--text-faint)' }}>ASSIGNED WAL LSN</div>
                <div style={{ fontWeight: 600, fontFamily: 'var(--font-num)', color: '#38bdf8' }}>{step.telemetry.assignedLsn}</div>
              </div>
              <div style={{ background: 'var(--surface-2)', padding: '6px 10px', borderRadius: 4 }}>
                <div style={{ fontSize: 10, color: 'var(--text-faint)' }}>CONCURRENCY WAITERS</div>
                <div style={{ fontWeight: 600, color: step.telemetry.queueWaiters.includes('Blocked') ? 'var(--danger)' : 'var(--text-muted)' }}>
                  {step.telemetry.queueWaiters}
                </div>
              </div>
            </div>
          </div>
        </div>

        {/* Right: Active Strict 2PL Lock Table & Theoretical Guarantees */}
        <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
          {/* Active Lock Table */}
          <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 14 }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 8 }}>
              <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: 0.5 }}>
                Active Strict 2PL Lock Table
              </span>
              <span className="badge" style={{ fontSize: 10, background: 'var(--accent-wash)', color: 'var(--accent)' }}>
                Serializable Schedule
              </span>
            </div>
            <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 11 }}>
              <thead>
                <tr style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-muted)', textAlign: 'left' }}>
                  <th style={{ padding: '4px 6px' }}>TxID</th>
                  <th style={{ padding: '4px 6px' }}>Client</th>
                  <th style={{ padding: '4px 6px' }}>Resource</th>
                  <th style={{ padding: '4px 6px' }}>Mode</th>
                  <th style={{ padding: '4px 6px', textAlign: 'right' }}>Status</th>
                </tr>
              </thead>
              <tbody>
                {step.lockTable.map((row) => (
                  <tr key={row.txId} style={{ borderBottom: '1px solid rgba(255,255,255,0.04)' }}>
                    <td style={{ padding: '6px', fontWeight: 700, fontFamily: 'var(--font-num)' }}>{row.txId}</td>
                    <td style={{ padding: '6px', color: 'var(--text-muted)' }}>{row.client}</td>
                    <td style={{ padding: '6px' }}>{row.resource}</td>
                    <td style={{ padding: '6px', fontFamily: 'var(--font-num)', color: row.mode.includes('EXCLUSIVE') ? '#f59e0b' : 'var(--text)' }}>
                      {row.mode}
                    </td>
                    <td style={{ padding: '6px', textAlign: 'right', fontWeight: 600, color: row.statusColor }}>
                      {row.status}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>

          {/* Strict 2PL Theoretical Guarantees Card */}
          <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 14 }}>
            <div style={{ fontSize: 11, fontWeight: 700, color: 'var(--accent)', textTransform: 'uppercase', letterSpacing: 0.5, marginBottom: 8 }}>
              Mathematical Guarantees of Strict 2PL
            </div>
            <div style={{ display: 'flex', flexDirection: 'column', gap: 6, fontSize: 11 }}>
              <div style={{ display: 'flex', gap: 8, alignItems: 'flex-start' }}>
                <span style={{ color: 'var(--success)', fontWeight: 700 }}>✓</span>
                <span style={{ color: 'var(--text-muted)' }}>
                  <strong style={{ color: 'var(--text)' }}>Conflict Serializability: </strong>
                  The precedence graph contains zero directed cycles. Schedules are equivalent to a serial execution.
                </span>
              </div>
              <div style={{ display: 'flex', gap: 8, alignItems: 'flex-start' }}>
                <span style={{ color: 'var(--success)', fontWeight: 700 }}>✓</span>
                <span style={{ color: 'var(--text-muted)' }}>
                  <strong style={{ color: 'var(--text)' }}>Strict Recoverability & No Cascading Aborts: </strong>
                  Because exclusive write locks are held until COMMIT, no transaction ever reads uncommitted changes. If a transaction aborts, rollback is isolated.
                </span>
              </div>
              <div style={{ display: 'flex', gap: 8, alignItems: 'flex-start' }}>
                <span style={{ color: 'var(--success)', fontWeight: 700 }}>✓</span>
                <span style={{ color: 'var(--text-muted)' }}>
                  <strong style={{ color: 'var(--text)' }}>WAL Precedence: </strong>
                  Log records are flushed via <code style={{ color: '#38bdf8' }}>fdatasync()</code> strictly prior to memory updates, preserving durable ACID recovery.
                </span>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}

// ──────────────────────────────────────────────────────────────────────────────
// 4. ARIES RECOVERY SIMULATOR (Live Data & 3-Phase ARIES C++ Sandbox)
// ──────────────────────────────────────────────────────────────────────────────

interface AriesStepData {
  step: number;
  title: string;
  phase: 'idle' | 'crash' | 'analysis' | 'redo' | 'undo' | 'restored';
  phaseTag: string;
  tagColor: string;
  tagBg: string;
  scannedLsn: number;
  desc: string;
  banner: string;
  bannerType: 'info' | 'danger' | 'warning' | 'success';
  txTable: Array<{ id: string; status: string; statusColor: string; lastLsn: number }>;
  dirtyPageTable: Array<{ pageId: string; recLsn: number; status: string }>;
  memoryValue: string;
  diskValue: string;
}

const DEFAULT_ACADEMIC_LOT: RawLot = {
  item_id: 'lot_academic',
  host_id: 'system',
  title: 'Lot A (Vintage Watch)',
  category: 'WATCHES',
  condition: 'EXCELLENT',
  starting_price: 50,
  status: 'OPEN',
  current_bid: 75,
  current_winner: 'T-101 (Bidder A)',
  end_time_ms: 0,
  top_bid_count: 1,
  total_bid_count: 2,
  image_urls: ['', '', ''],
  top_bids: [],
};

function AriesRecoverySimulator({ items = [] }: { items?: RawLot[] }) {
  const [internalItems, setInternalItems] = useState<RawLot[]>(items);
  const [selectedId, setSelectedId] = useState<string>('');
  const [inflightUser, setInflightUser] = useState<string>('Vinayak_CrashTest');
  const [inflightBidInput, setInflightBidInput] = useState<string>('');
  const [isLiveMode, setIsLiveMode] = useState<boolean>(true);

  // Stepper & Playback State
  const [currentStep, setCurrentStep] = useState(0);
  const [isPlaying, setIsPlaying] = useState(false);

  // C++ Engine Sandbox Verification State
  const [isSimulatingCpp, setIsSimulatingCpp] = useState(false);
  const [cppResult, setCppResult] = useState<LiveRecoverySimResponse | null>(null);
  const [cppError, setCppError] = useState<string | null>(null);
  const [showCppTrace, setShowCppTrace] = useState<boolean>(true);

  // Sync internal items if prop changes or fetch if empty
  useEffect(() => {
    if (items.length > 0) {
      setInternalItems(items);
      if (!selectedId) setSelectedId(items[0].item_id);
    } else {
      api.items().then((res) => {
        if (Array.isArray(res) && res.length > 0) {
          setInternalItems(res);
          if (!selectedId) setSelectedId(res[0].item_id);
        }
      }).catch(() => {});
    }
  }, [items]);

  const activeLot = useMemo(() => {
    if (!isLiveMode) return DEFAULT_ACADEMIC_LOT;
    if (internalItems.length === 0) return DEFAULT_ACADEMIC_LOT;
    return internalItems.find((i) => i.item_id === selectedId) || internalItems[0] || DEFAULT_ACADEMIC_LOT;
  }, [isLiveMode, internalItems, selectedId]);

  const committedPrice = activeLot.current_bid > 0 ? activeLot.current_bid : activeLot.starting_price;
  const committedWinner = activeLot.current_winner || activeLot.host_id || 'Committed Winner';

  const suggestedInflight = useMemo(() => {
    if (inflightBidInput) {
      const parsed = parseFloat(inflightBidInput);
      if (!isNaN(parsed) && parsed > committedPrice) return parsed;
    }
    return committedPrice < 100 ? committedPrice + 25 : committedPrice + 150;
  }, [inflightBidInput, committedPrice]);

  // Dynamically compute the WAL Tape Records tailored to the live item
  const tapeRecords = useMemo(() => {
    const lotTitle = activeLot.title;
    return [
      { lsn: 101, tx: 'Tx #101', type: 'START', info: `Tx #101 (${committedWinner}) started`, badgeColor: '#38bdf8' },
      { lsn: 102, tx: 'Tx #101', type: 'UPDATE', info: `${lotTitle}: Bid $${committedPrice.toLocaleString()} (${committedWinner})`, badgeColor: 'var(--accent)' },
      { lsn: 103, tx: 'Tx #101', type: 'COMMIT', info: `Tx #101 committed (Durable on disk)`, badgeColor: 'var(--success)' },
      { lsn: 104, tx: 'SYS', type: 'CHECKPOINT', info: 'Fuzzy checkpoint (DPT clean, Sync point)', badgeColor: '#a855f7' },
      { lsn: 105, tx: 'Tx #102', type: 'START', info: `Tx #102 (${inflightUser}) started`, badgeColor: '#38bdf8' },
      { lsn: 106, tx: 'Tx #102', type: 'UPDATE', info: `${lotTitle}: $${committedPrice.toLocaleString()} → $${suggestedInflight.toLocaleString()} (Dirty in RAM)`, badgeColor: '#f59e0b' },
      { lsn: 107, tx: 'CRASH', type: 'CRASH_POINT', info: '⚡ Sudden Power Loss / SIGKILL (RAM Wiped)', badgeColor: 'var(--danger)' },
      { lsn: 108, tx: 'Tx #102', type: 'CLR_UNDO', info: `CLR: Undo ${lotTitle} back to $${committedPrice.toLocaleString()} (${committedWinner})`, badgeColor: '#38bdf8' },
      { lsn: 109, tx: 'Tx #102', type: 'END', info: 'Tx #102 Aborted & Cleared (Safe)', badgeColor: 'var(--success)' },
    ];
  }, [activeLot, committedPrice, committedWinner, suggestedInflight, inflightUser]);

  // Dynamically compute the 8 ARIES Steps tailored to the live item with stable, non-jumping row structures
  const steps: AriesStepData[] = useMemo(() => {
    const lotName = activeLot.title;
    const pageStr = `Page 0 (${activeLot.item_id})`;
    return [
      {
        step: 0,
        title: `Pre-Crash Steady State: ${lotName} at $${committedPrice.toLocaleString()}`,
        phase: 'idle',
        phaseTag: 'NORMAL RUNTIME',
        tagColor: 'var(--success)',
        tagBg: 'rgba(16, 185, 129, 0.12)',
        scannedLsn: 106,
        desc: `Tx #101 (${committedWinner}) has committed at $${committedPrice.toLocaleString()} and is persisted. Active competitor Tx #102 (${inflightUser}) places a new high bid of $${suggestedInflight.toLocaleString()}. In accordance with the WAL protocol, an UPDATE record is appended to the WAL buffer, and ${pageStr} is dirtied in volatile Buffer Pool memory.`,
        banner: `System Operating Normally: Strict 2PL exclusive lock held on ${lotName}. WAL log buffer dirty in RAM.`,
        bannerType: 'info',
        txTable: [
          { id: 'Tx #101', status: 'COMMITTED', statusColor: 'var(--success)', lastLsn: 103 },
          { id: 'Tx #102', status: 'ACTIVE (UNCOMMITTED)', statusColor: 'var(--accent)', lastLsn: 106 },
        ],
        dirtyPageTable: [{ pageId: pageStr, recLsn: 106, status: 'DIRTY IN RAM' }],
        memoryValue: `$${suggestedInflight.toLocaleString()} (${inflightUser})`,
        diskValue: `$${committedPrice.toLocaleString()} (${committedWinner})`,
      },
      {
        step: 1,
        title: `⚡ Sudden Power Failure / Process Crash on ${lotName}!`,
        phase: 'crash',
        phaseTag: 'CRASH OCCURRED',
        tagColor: 'var(--danger)',
        tagBg: 'var(--danger-wash)',
        scannedLsn: 107,
        desc: `Operating system crash or server termination (SIGKILL)! All volatile RAM (Buffer Pool frames, transaction lock tables) is erased instantly. The dirty in-flight bid of $${suggestedInflight.toLocaleString()} was never committed. Only persistent walnut.wal and database files on disk survive!`,
        banner: `CRITICAL FAILURE: Volatile RAM wiped. Recovery for ${lotName} relies exclusively on persistent WAL records.`,
        bannerType: 'danger',
        txTable: [
          { id: 'Tx #101', status: 'COMMITTED (ON DISK)', statusColor: 'var(--success)', lastLsn: 103 },
          { id: 'Tx #102', status: 'ACTIVE LOSER (RAM WIPED)', statusColor: 'var(--danger)', lastLsn: 106 },
        ],
        dirtyPageTable: [{ pageId: pageStr, recLsn: 106, status: 'DIRTY (RAM WIPED)' }],
        memoryValue: '[RAM WIPED / EMPTY]',
        diskValue: `$${committedPrice.toLocaleString()} (Stale disk page)`,
      },
      {
        step: 2,
        title: `Phase 1 [Analysis]: Locate Checkpoint & Scan WAL for ${lotName}`,
        phase: 'analysis',
        phaseTag: 'PHASE 1: ANALYSIS',
        tagColor: 'var(--accent)',
        tagBg: 'var(--accent-wash)',
        scannedLsn: 104,
        desc: `WALnut engine restarts via FileManager::initialize(). RecoveryManager scans backward from the end of walnut.wal to find the last Checkpoint at LSN 104. This establishes the initial baseline snapshot for the Active Transaction Table (ATT) and Dirty Page Table (DPT).`,
        banner: 'Checkpoint Found at LSN 104: Loading baseline snapshot before analyzing in-flight bids.',
        bannerType: 'warning',
        txTable: [
          { id: 'Tx #101', status: 'COMMITTED (CHECKPOINT)', statusColor: 'var(--success)', lastLsn: 103 },
          { id: 'Tx #102', status: 'SCANNING WAL STREAM', statusColor: 'var(--accent)', lastLsn: 106 },
        ],
        dirtyPageTable: [{ pageId: pageStr, recLsn: 106, status: 'RECONSTRUCTING' }],
        memoryValue: '[Initializing Recovery Cache]',
        diskValue: `$${committedPrice.toLocaleString()}`,
      },
      {
        step: 3,
        title: `Phase 1 [Analysis]: Scan Forward & Flag Loser Tx #102`,
        phase: 'analysis',
        phaseTag: 'PHASE 1: ANALYSIS COMPLETE',
        tagColor: 'var(--accent)',
        tagBg: 'var(--accent-wash)',
        scannedLsn: 106,
        desc: `Analysis scans forward from Checkpoint (LSN 104) to Crash Point (LSN 107). It discovers Tx #102 started at LSN 105 and updated ${lotName} at LSN 106, but NEVER committed! Tx #102 is flagged as a LOSER transaction that must be rolled back. ${pageStr} is registered in DPT with RecLSN=106.`,
        banner: `Analysis Complete: Loser transaction identified (Tx #102). Dirty Page ${pageStr} queued for Redo.`,
        bannerType: 'warning',
        txTable: [
          { id: 'Tx #101', status: 'COMMITTED', statusColor: 'var(--success)', lastLsn: 103 },
          { id: 'Tx #102', status: 'LOSER (MUST UNDO)', statusColor: 'var(--danger)', lastLsn: 106 },
        ],
        dirtyPageTable: [{ pageId: pageStr, recLsn: 106, status: 'QUEUED FOR REDO' }],
        memoryValue: `[Rebuilding ${pageStr}]`,
        diskValue: `$${committedPrice.toLocaleString()}`,
      },
      {
        step: 4,
        title: `Phase 2 [Redo]: Repeat History from Min(RecLSN)`,
        phase: 'redo',
        phaseTag: 'PHASE 2: REDO PASS',
        tagColor: '#f59e0b',
        tagBg: 'rgba(245, 158, 11, 0.12)',
        scannedLsn: 102,
        desc: `ARIES begins the Redo pass at the smallest RecLSN across all dirty pages (LSN 102). ARIES strictly "Repeats History": it reapplies logged modifications forward to reconstruct the exact pre-crash state, regardless of whether the transaction committed or aborted.`,
        banner: `Repeating History: Replaying committed update Tx #101 (${lotName}: $${committedPrice.toLocaleString()} by ${committedWinner}).`,
        bannerType: 'warning',
        txTable: [
          { id: 'Tx #101', status: 'COMMITTED (REPLAYED)', statusColor: 'var(--success)', lastLsn: 103 },
          { id: 'Tx #102', status: 'LOSER (MUST UNDO)', statusColor: 'var(--danger)', lastLsn: 106 },
        ],
        dirtyPageTable: [{ pageId: pageStr, recLsn: 102, status: 'REDOING LSN 102' }],
        memoryValue: `$${committedPrice.toLocaleString()} (Tx #101 Replayed)`,
        diskValue: `$${committedPrice.toLocaleString()}`,
      },
      {
        step: 5,
        title: `Phase 2 [Redo]: In-Flight Bid Reapplied to Memory`,
        phase: 'redo',
        phaseTag: 'PHASE 2: REDO COMPLETE',
        tagColor: '#f59e0b',
        tagBg: 'rgba(245, 158, 11, 0.12)',
        scannedLsn: 106,
        desc: `Redo applies LSN 106 to ${pageStr} in the Buffer Pool, setting PageLSN = 106. The database state in RAM now matches the exact microsecond at the moment of the crash ($${suggestedInflight.toLocaleString()} by ${inflightUser}).`,
        banner: 'Redo Complete: System state faithfully recreated in memory. Ready for isolated Undo pass.',
        bannerType: 'warning',
        txTable: [
          { id: 'Tx #101', status: 'COMMITTED', statusColor: 'var(--success)', lastLsn: 103 },
          { id: 'Tx #102', status: 'ACTIVE LOSER', statusColor: 'var(--danger)', lastLsn: 106 },
        ],
        dirtyPageTable: [{ pageId: pageStr, recLsn: 106, status: 'REDO COMPLETE' }],
        memoryValue: `$${suggestedInflight.toLocaleString()} (Crash State Recreated)`,
        diskValue: `$${committedPrice.toLocaleString()}`,
      },
      {
        step: 6,
        title: `Phase 3 [Undo]: Rollback Loser & Write Compensation Log Record (CLR)`,
        phase: 'undo',
        phaseTag: 'PHASE 3: UNDO PASS',
        tagColor: 'var(--danger)',
        tagBg: 'var(--danger-wash)',
        scannedLsn: 108,
        desc: `ARIES scans backward from crash point along the prev_lsn chain of loser Tx #102. The uncommitted bid on ${lotName} is undone: the before_image is applied, reverting the price from $${suggestedInflight.toLocaleString()} back to $${committedPrice.toLocaleString()} (${committedWinner}). A durable Compensation Log Record (CLR LSN 108) with undo_next_lsn is written to the WAL!`,
        banner: 'Undo in Progress: CLR written for Tx #102. CLRs ensure that if another crash occurs during recovery, work is never repeated.',
        bannerType: 'danger',
        txTable: [
          { id: 'Tx #101', status: 'COMMITTED', statusColor: 'var(--success)', lastLsn: 103 },
          { id: 'Tx #102', status: 'ROLLING BACK (CLR)', statusColor: '#38bdf8', lastLsn: 108 },
        ],
        dirtyPageTable: [{ pageId: pageStr, recLsn: 108, status: 'UNDONE VIA CLR' }],
        memoryValue: `$${committedPrice.toLocaleString()} (Reverted by CLR)`,
        diskValue: `$${committedPrice.toLocaleString()}`,
      },
      {
        step: 7,
        title: `Phase 3 [Undo]: ABORT Record & Live Lot ${lotName} Fully Restored ✓`,
        phase: 'restored',
        phaseTag: 'FULL RECOVERY ✓',
        tagColor: 'var(--success)',
        tagBg: 'rgba(16, 185, 129, 0.12)',
        scannedLsn: 109,
        desc: `An ABORT record (LSN 109) is appended for Tx #102. The loser transaction is purged from the transaction table, all locks on ${lotName} are released, and ${pageStr} is flushed cleanly to disk. The live auction lot retains its legitimate durable winner (${committedWinner} at $${committedPrice.toLocaleString()}) with zero data corruption!`,
        banner: 'RECOVERY SUCCESSFUL: Phantom bid discarded. Durable winner preserved. System fully operational.',
        bannerType: 'success',
        txTable: [
          { id: 'Tx #101', status: 'COMMITTED', statusColor: 'var(--success)', lastLsn: 103 },
          { id: 'Tx #102', status: 'ABORTED / RESOLVED', statusColor: 'var(--text-muted)', lastLsn: 109 },
        ],
        dirtyPageTable: [{ pageId: pageStr, recLsn: 0, status: 'CLEAN & FLUSHED' }],
        memoryValue: `$${committedPrice.toLocaleString()} (Durable Final)`,
        diskValue: `$${committedPrice.toLocaleString()} (Synchronized to Disk)`,
      },
    ];
  }, [activeLot, committedPrice, committedWinner, suggestedInflight, inflightUser]);

  useEffect(() => {
    if (!isPlaying) return;
    const timer = setInterval(() => {
      setCurrentStep((prev) => {
        if (prev >= steps.length - 1) {
          setIsPlaying(false);
          return prev;
        }
        return prev + 1;
      });
    }, 2400);
    return () => clearInterval(timer);
  }, [isPlaying, steps.length]);

  const stepData = steps[currentStep] || steps[0];

  function handlePrev() {
    setIsPlaying(false);
    setCurrentStep((prev) => Math.max(0, prev - 1));
  }

  function handleNext() {
    setIsPlaying(false);
    setCurrentStep((prev) => Math.min(steps.length - 1, prev + 1));
  }

  function handleReset() {
    setIsPlaying(false);
    setCurrentStep(0);
  }

  function handleTogglePlay() {
    if (currentStep >= steps.length - 1) {
      setCurrentStep(0);
      setIsPlaying(true);
    } else {
      setIsPlaying(!isPlaying);
    }
  }

  function handleInjectCrash() {
    setIsPlaying(false);
    setCurrentStep(1); // Jump straight to Crash Point
  }

  // Real C++ Engine Verification Sandbox Runner
  async function runCppSimulation() {
    setIsSimulatingCpp(true);
    setCppError(null);
    try {
      const res = await api.simulateLiveRecovery({
        item_id: activeLot.item_id,
        inflight_user: inflightUser,
        inflight_amount: suggestedInflight,
      });
      setCppResult(res);
      setShowCppTrace(true);
    } catch (err: any) {
      setCppError(err?.message || 'Failed to execute C++ recovery sandbox.');
    } finally {
      setIsSimulatingCpp(false);
    }
  }

  return (
    <div className="card" style={{ padding: 24, marginBottom: 28 }}>
      {/* Title & Toolbar - Anchored on top-right with no layout shifts */}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', gap: 16, marginBottom: 16 }}>
        <div style={{ flex: 1, minWidth: 0 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
            <h2 className="section-title" style={{ margin: 0, fontSize: 17, whiteSpace: 'nowrap' }}>
              Write-Ahead Log (WAL) · ARIES Crash Recovery Simulation
            </h2>
            <span
              style={{
                fontSize: 10,
                fontWeight: 700,
                padding: '3px 9px',
                borderRadius: 12,
                background: stepData.tagBg,
                color: stepData.tagColor,
                border: `1px solid ${stepData.tagColor}`,
                textTransform: 'uppercase',
                letterSpacing: 0.5,
                whiteSpace: 'nowrap',
              }}
            >
              {stepData.phaseTag}
            </span>
          </div>
          <p style={{ fontSize: 12, color: 'var(--text-muted)', margin: '4px 0 0 0', lineHeight: 1.4 }}>
            Interactive recovery of live auction lots using ARIES (Analysis → Redo → Undo + CLR) and verified with the C++ RecoveryManager.
          </p>
        </div>

        {/* Action Controls - Permanently anchored on top-right, never wrapping */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexShrink: 0 }}>
          <button
            className="btn btn-ghost btn-sm"
            onClick={handlePrev}
            disabled={currentStep === 0}
            style={{ fontSize: 12, padding: '6px 12px' }}
          >
            ◀ Prev
          </button>
          <button
            className={`btn btn-sm ${isPlaying ? 'btn-danger' : 'btn-primary'}`}
            onClick={handleTogglePlay}
            style={{ fontSize: 12, padding: '6px 14px', minWidth: 96 }}
          >
            {isPlaying ? '⏸ Pause' : currentStep >= steps.length - 1 ? '↺ Replay' : '▶ Auto-Play'}
          </button>
          <button
            className="btn btn-ghost btn-sm"
            onClick={handleNext}
            disabled={currentStep === steps.length - 1}
            style={{ fontSize: 12, padding: '6px 12px' }}
          >
            Next ▶
          </button>
          <button
            className="btn btn-ghost btn-sm"
            onClick={handleReset}
            style={{ fontSize: 12, padding: '6px 10px' }}
          >
            ↺ Reset
          </button>
        </div>
      </div>

      {/* Mode Switcher & Live Lot Binding Card */}
      <div style={{ background: 'var(--surface-2)', border: '1px solid var(--border)', borderRadius: 8, padding: '14px 16px', marginBottom: 18 }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: 12, marginBottom: 12 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
            <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: 0.5 }}>
              Data Binding Mode:
            </span>
            <button
              className={`btn btn-sm ${isLiveMode ? 'btn-primary' : 'btn-ghost'}`}
              onClick={() => { setIsLiveMode(true); setCurrentStep(0); }}
              style={{ fontSize: 11, padding: '4px 10px' }}
            >
              📦 Live Auction Lot
            </button>
            <button
              className={`btn btn-sm ${!isLiveMode ? 'btn-primary' : 'btn-ghost'}`}
              onClick={() => { setIsLiveMode(false); setCurrentStep(0); }}
              style={{ fontSize: 11, padding: '4px 10px' }}
            >
              📚 Academic Model (T-101)
            </button>
          </div>

          {/* C++ Verification Trigger */}
          <button
            className="btn btn-ghost btn-sm"
            onClick={runCppSimulation}
            disabled={isSimulatingCpp}
            style={{
              fontSize: 11,
              padding: '5px 12px',
              color: 'var(--accent)',
              border: '1px solid rgba(212, 160, 23, 0.3)',
              background: 'rgba(212, 160, 23, 0.08)',
              fontWeight: 700,
            }}
          >
            {isSimulatingCpp ? '⏳ Running C++ Sandbox...' : '⚡ Verify with Real C++ Recovery Engine'}
          </button>
        </div>

        {/* Live Lot Configuration Controls */}
        {isLiveMode && (
          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(220px, 1fr))', gap: 12, alignItems: 'center' }}>
            <div>
              <label style={{ fontSize: 11, color: 'var(--text-muted)', display: 'block', marginBottom: 4 }}>
                Target Live Lot:
              </label>
              <select
                className="input"
                value={selectedId}
                onChange={(e) => { setSelectedId(e.target.value); setCurrentStep(0); }}
                style={{ width: '100%', fontSize: 12, padding: '6px 8px', background: 'var(--surface)', color: 'var(--text)' }}
              >
                {internalItems.map((it) => (
                  <option key={it.item_id} value={it.item_id}>
                    {it.title} (${(it.current_bid > 0 ? it.current_bid : it.starting_price).toLocaleString()} by {it.current_winner || it.host_id || 'Host'})
                  </option>
                ))}
              </select>
            </div>

            <div>
              <label style={{ fontSize: 11, color: 'var(--text-muted)', display: 'block', marginBottom: 4 }}>
                In-Flight Competitor:
              </label>
              <input
                type="text"
                className="input"
                value={inflightUser}
                onChange={(e) => setInflightUser(e.target.value)}
                placeholder="Competitor username"
                style={{ width: '100%', fontSize: 12, padding: '6px 8px', background: 'var(--surface)', color: 'var(--text)' }}
              />
            </div>

            <div>
              <label style={{ fontSize: 11, color: 'var(--text-muted)', display: 'block', marginBottom: 4 }}>
                Uncommitted Crash Bid ($):
              </label>
              <input
                type="number"
                className="input"
                value={inflightBidInput || suggestedInflight}
                onChange={(e) => setInflightBidInput(e.target.value)}
                style={{ width: '100%', fontSize: 12, padding: '6px 8px', background: 'var(--surface)', color: 'var(--text)' }}
              />
            </div>

            <div style={{ alignSelf: 'flex-end' }}>
              <button
                className="btn btn-danger btn-sm"
                onClick={handleInjectCrash}
                style={{ width: '100%', fontSize: 12, padding: '7px 12px', fontWeight: 700 }}
              >
                💥 Crash In-Flight Bid
              </button>
            </div>
          </div>
        )}
      </div>

      {/* Real C++ Verification Diagnostic Banner */}
      {cppResult && (
        <div
          style={{
            background: 'rgba(16, 185, 129, 0.08)',
            border: '1px solid rgba(16, 185, 129, 0.3)',
            borderRadius: 8,
            padding: 16,
            marginBottom: 18,
          }}
        >
          <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: 10, marginBottom: 10 }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
              <span style={{ color: 'var(--success)', fontWeight: 800, fontSize: 15 }}>✓</span>
              <strong style={{ fontSize: 13, color: 'var(--text)' }}>
                Executed Real C++ ARIES RecoveryManager (<code style={{ color: 'var(--accent)' }}>src/wal/recovery_manager.cpp</code>)
              </strong>
            </div>
            <button
              className="btn btn-ghost btn-sm"
              onClick={() => setShowCppTrace(!showCppTrace)}
              style={{ fontSize: 11, padding: '3px 8px' }}
            >
              {showCppTrace ? '▲ Hide Binary WAL' : '▼ Inspect Binary WAL'}
            </button>
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(130px, 1fr))', gap: 10, marginBottom: 12 }}>
            <div style={{ background: 'var(--surface)', padding: '8px 12px', borderRadius: 6, border: '1px solid var(--border)' }}>
              <div style={{ fontSize: 10, color: 'var(--text-muted)' }}>RECORDS ANALYZED</div>
              <div style={{ fontSize: 16, fontWeight: 800, fontFamily: 'var(--font-num)', color: 'var(--accent)', marginTop: 2 }}>
                {cppResult.stats.records_analyzed}
              </div>
            </div>
            <div style={{ background: 'var(--surface)', padding: '8px 12px', borderRadius: 6, border: '1px solid var(--border)' }}>
              <div style={{ fontSize: 10, color: 'var(--text-muted)' }}>RECORDS REDONE</div>
              <div style={{ fontSize: 16, fontWeight: 800, fontFamily: 'var(--font-num)', color: '#f59e0b', marginTop: 2 }}>
                {cppResult.stats.records_redone}
              </div>
            </div>
            <div style={{ background: 'var(--surface)', padding: '8px 12px', borderRadius: 6, border: '1px solid var(--border)' }}>
              <div style={{ fontSize: 10, color: 'var(--text-muted)' }}>RECORDS UNDONE</div>
              <div style={{ fontSize: 16, fontWeight: 800, fontFamily: 'var(--font-num)', color: 'var(--danger)', marginTop: 2 }}>
                {cppResult.stats.records_undone}
              </div>
            </div>
            <div style={{ background: 'var(--surface)', padding: '8px 12px', borderRadius: 6, border: '1px solid var(--border)' }}>
              <div style={{ fontSize: 10, color: 'var(--text-muted)' }}>CLRs WRITTEN</div>
              <div style={{ fontSize: 16, fontWeight: 800, fontFamily: 'var(--font-num)', color: '#38bdf8', marginTop: 2 }}>
                {cppResult.stats.clrs_written}
              </div>
            </div>
            <div style={{ background: 'var(--surface)', padding: '8px 12px', borderRadius: 6, border: '1px solid var(--border)' }}>
              <div style={{ fontSize: 10, color: 'var(--text-muted)' }}>LOSER TXNS FOUND</div>
              <div style={{ fontSize: 16, fontWeight: 800, fontFamily: 'var(--font-num)', color: 'var(--danger)', marginTop: 2 }}>
                {cppResult.stats.loser_txn_count}
              </div>
            </div>
            <div style={{ background: 'var(--surface)', padding: '8px 12px', borderRadius: 6, border: '1px solid var(--border)' }}>
              <div style={{ fontSize: 10, color: 'var(--text-muted)' }}>RESTORED PRICE</div>
              <div style={{ fontSize: 16, fontWeight: 800, fontFamily: 'var(--font-num)', color: 'var(--success)', marginTop: 2 }}>
                ${cppResult.restored_price.toLocaleString()}
              </div>
            </div>
          </div>

          {showCppTrace && (
            <div style={{ background: 'var(--surface)', borderRadius: 6, padding: 10, border: '1px solid var(--border)', overflowX: 'auto' }}>
              <div style={{ fontSize: 11, fontWeight: 700, color: 'var(--text-muted)', marginBottom: 6 }}>
                RAW C++ BINARY WAL RECORDS PARSED & PROCESSED BY RECOVERY ENGINE:
              </div>
              <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 11 }}>
                <thead>
                  <tr style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-muted)', textAlign: 'left' }}>
                    <th style={{ padding: '4px 6px' }}>LSN</th>
                    <th style={{ padding: '4px 6px' }}>PrevLSN</th>
                    <th style={{ padding: '4px 6px' }}>Tx ID</th>
                    <th style={{ padding: '4px 6px' }}>Type</th>
                    <th style={{ padding: '4px 6px' }}>Page ID</th>
                    <th style={{ padding: '4px 6px' }}>UndoNextLSN</th>
                    <th style={{ padding: '4px 6px' }}>Action</th>
                  </tr>
                </thead>
                <tbody>
                  {cppResult.wal_records.map((r, i) => (
                    <tr key={i} style={{ borderBottom: '1px solid rgba(255,255,255,0.04)' }}>
                      <td style={{ padding: '4px 6px', fontFamily: 'var(--font-num)', color: 'var(--accent)', fontWeight: 700 }}>#{r.lsn}</td>
                      <td style={{ padding: '4px 6px', fontFamily: 'var(--font-num)', color: 'var(--text-muted)' }}>#{r.prev_lsn}</td>
                      <td style={{ padding: '4px 6px', fontFamily: 'var(--font-num)' }}>Tx {r.txn_id}</td>
                      <td style={{ padding: '4px 6px', fontWeight: 700, color: r.type === 'CLR' ? '#38bdf8' : r.type === 'UPDATE' ? '#f59e0b' : r.type === 'ABORT' ? 'var(--danger)' : 'var(--success)' }}>
                        {r.type}
                      </td>
                      <td style={{ padding: '4px 6px', fontFamily: 'var(--font-num)', color: 'var(--text-muted)' }}>Page {r.page_id}</td>
                      <td style={{ padding: '4px 6px', fontFamily: 'var(--font-num)', color: r.undo_next_lsn > 0 ? '#38bdf8' : 'var(--text-faint)' }}>
                        {r.undo_next_lsn > 0 ? `#${r.undo_next_lsn}` : '0'}
                      </td>
                      <td style={{ padding: '4px 6px', fontSize: 10, color: 'var(--text-muted)' }}>
                        {r.type === 'CLR' ? 'Emitted during Undo pass to make rollback idempotent' : r.type === 'ABORT' ? 'Loser transaction cleanly resolved' : 'Replayed / Verified'}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </div>
      )}

      {cppError && (
        <div style={{ padding: '8px 12px', borderRadius: 6, background: 'var(--danger-wash)', color: 'var(--danger)', fontSize: 12, marginBottom: 16 }}>
          C++ Simulation Error: {cppError}
        </div>
      )}

      {/* Stepper Dots & Quick Jumps */}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: 10, padding: '10px 14px', background: 'var(--surface-2)', borderRadius: 8, border: '1px solid var(--border)', marginBottom: 16 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 6, flexWrap: 'wrap' }}>
          <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--text-muted)', marginRight: 6 }}>
            STEP {currentStep + 1}/8:
          </span>
          <div className="wfg-dots">
            {steps.map((s, idx) => (
              <button
                key={idx}
                className={`wfg-dot ${currentStep === idx ? 'active' : ''}`}
                onClick={() => { setIsPlaying(false); setCurrentStep(idx); }}
                title={`Step ${idx + 1}: ${s.title}`}
                style={{
                  width: currentStep === idx ? 22 : 12,
                  height: 12,
                  borderRadius: 6,
                  transition: 'all 200ms ease',
                  background: currentStep === idx ? 'var(--accent)' : currentStep > idx ? 'var(--border-strong)' : 'rgba(255,255,255,0.1)',
                }}
              />
            ))}
          </div>
        </div>

        {/* Quick Scenario Shortcuts */}
        <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap' }}>
          <button
            className="btn btn-ghost btn-sm"
            onClick={() => { setIsPlaying(false); setCurrentStep(1); }}
            style={{ fontSize: 11, padding: '4px 8px', color: currentStep === 1 ? 'var(--danger)' : 'var(--text-muted)' }}
          >
            ⚡ Crash (Step 2)
          </button>
          <button
            className="btn btn-ghost btn-sm"
            onClick={() => { setIsPlaying(false); setCurrentStep(3); }}
            style={{ fontSize: 11, padding: '4px 8px', color: currentStep === 3 ? 'var(--accent)' : 'var(--text-muted)' }}
          >
            🔍 Analysis (Step 4)
          </button>
          <button
            className="btn btn-ghost btn-sm"
            onClick={() => { setIsPlaying(false); setCurrentStep(5); }}
            style={{ fontSize: 11, padding: '4px 8px', color: currentStep === 5 ? '#f59e0b' : 'var(--text-muted)' }}
          >
            ⏩ Redo (Step 6)
          </button>
          <button
            className="btn btn-ghost btn-sm"
            onClick={() => { setIsPlaying(false); setCurrentStep(6); }}
            style={{ fontSize: 11, padding: '4px 8px', color: currentStep === 6 ? 'var(--danger)' : 'var(--text-muted)' }}
          >
            ⏪ Undo (Step 7)
          </button>
          <button
            className="btn btn-ghost btn-sm"
            onClick={() => { setIsPlaying(false); setCurrentStep(7); }}
            style={{ fontSize: 11, padding: '4px 8px', color: currentStep === 7 ? 'var(--success)' : 'var(--text-muted)' }}
          >
            ✓ Restored (Step 8)
          </button>
        </div>
      </div>

      {/* Sequenced WAL Log Tape */}
      <div style={{ marginBottom: 18 }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 6 }}>
          <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: 0.5 }}>
            Sequenced Write-Ahead Log Tape (LSN Stream)
          </span>
          <span style={{ fontSize: 10, color: 'var(--text-faint)' }}>Active Cursor: LSN #{stepData.scannedLsn}</span>
        </div>

        <div className="aries-tape-wrap">
          {tapeRecords.map((rec) => {
            const isScanned = stepData.scannedLsn === rec.lsn;
            const isCrash = rec.type === 'CRASH_POINT';
            const isClr = rec.type === 'CLR_UNDO';

            let cardClass = 'aries-card';
            if (isScanned) cardClass += ' active-scan';
            if (isCrash) cardClass += ' crash';
            if (isClr) cardClass += ' clr';

            return (
              <div key={rec.lsn} className={cardClass}>
                {isScanned && (
                  <div
                    style={{
                      position: 'absolute',
                      top: -10,
                      left: '50%',
                      transform: 'translateX(-50%)',
                      background: 'var(--accent)',
                      color: '#101216',
                      fontSize: 9,
                      fontWeight: 800,
                      padding: '1px 6px',
                      borderRadius: 4,
                      letterSpacing: '0.05em',
                      whiteSpace: 'nowrap',
                    }}
                  >
                    SCANNER ▲
                  </div>
                )}
                <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 4 }}>
                  <span style={{ fontFamily: 'var(--font-num)', fontSize: 11, color: isCrash ? 'var(--danger)' : 'var(--accent)', fontWeight: 700 }}>
                    LSN #{rec.lsn}
                  </span>
                  <span style={{ fontSize: 9, color: 'var(--text-faint)' }}>{rec.tx}</span>
                </div>
                <div style={{ fontSize: 11, fontWeight: 700, color: rec.badgeColor, marginBottom: 2 }}>
                  {rec.type}
                </div>
                <div style={{ fontSize: 10, color: 'var(--text-muted)', lineHeight: 1.35 }}>
                  {rec.info}
                </div>
              </div>
            );
          })}
        </div>
      </div>

      {/* Main Grid: Step Diagnostics Left + Tables Right */}
      <div className="wfg-grid">
        {/* LEFT: Step Details & Memory vs Disk State */}
        <div style={{ display: 'flex', flexDirection: 'column', gap: 14 }}>
          {/* Step Explanation Card */}
          <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 16 }}>
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 6 }}>
              <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--accent)', textTransform: 'uppercase', letterSpacing: 0.5 }}>
                Step {stepData.step + 1} of 8: {stepData.phaseTag}
              </span>
              <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>
                Cursor at LSN #{stepData.scannedLsn}
              </span>
            </div>
            <h3 style={{ fontSize: 15, fontWeight: 700, margin: '0 0 8px 0', color: 'var(--text)' }}>
              {stepData.title}
            </h3>
            <p style={{ fontSize: 12, lineHeight: 1.55, color: 'var(--text-muted)', margin: 0 }}>
              {stepData.desc}
            </p>

            {/* Banner Callout */}
            <div
              style={{
                marginTop: 12,
                padding: '8px 12px',
                borderRadius: 6,
                fontSize: 12,
                fontWeight: 600,
                background:
                  stepData.bannerType === 'danger'
                    ? 'var(--danger-wash)'
                    : stepData.bannerType === 'warning'
                    ? 'rgba(245, 158, 11, 0.1)'
                    : stepData.bannerType === 'success'
                    ? 'rgba(16, 185, 129, 0.1)'
                    : 'var(--surface-2)',
                color:
                  stepData.bannerType === 'danger'
                    ? 'var(--danger)'
                    : stepData.bannerType === 'warning'
                    ? '#f59e0b'
                    : stepData.bannerType === 'success'
                    ? 'var(--success)'
                    : 'var(--accent)',
                borderLeft: `3px solid ${
                  stepData.bannerType === 'danger'
                    ? 'var(--danger)'
                    : stepData.bannerType === 'warning'
                    ? '#f59e0b'
                    : stepData.bannerType === 'success'
                    ? 'var(--success)'
                    : 'var(--accent)'
                }`,
              }}
            >
              {stepData.banner}
            </div>
          </div>

          {/* Buffer Pool (RAM) vs Disk Value Comparison */}
          <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 14 }}>
            <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: 0.5, display: 'block', marginBottom: 10 }}>
              Physical Data Convergence: Volatile RAM vs On-Disk
            </span>
            <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12 }}>
              <div style={{ background: 'var(--surface-2)', padding: 10, borderRadius: 6, border: '1px solid var(--border)' }}>
                <div style={{ fontSize: 10, color: 'var(--text-muted)', textTransform: 'uppercase' }}>Buffer Pool (RAM Frame P-0)</div>
                <div style={{ fontFamily: 'var(--font-num)', fontWeight: 700, fontSize: 14, color: 'var(--accent)', marginTop: 4 }}>
                  {stepData.memoryValue}
                </div>
              </div>
              <div style={{ background: 'var(--surface-2)', padding: 10, borderRadius: 6, border: '1px solid var(--border)' }}>
                <div style={{ fontSize: 10, color: 'var(--text-muted)', textTransform: 'uppercase' }}>Database File (Disk Page P-0)</div>
                <div style={{ fontFamily: 'var(--font-num)', fontWeight: 700, fontSize: 14, color: 'var(--success)', marginTop: 4 }}>
                  {stepData.diskValue}
                </div>
              </div>
            </div>
          </div>
        </div>

        {/* RIGHT: Active Transaction Table & Dirty Page Table */}
        <div style={{ display: 'flex', flexDirection: 'column', gap: 14 }}>
          {/* Transaction Table */}
          <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 14 }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 8 }}>
              <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: 0.5 }}>
                Active Transaction Table (ATT / RAM State)
              </span>
              <span style={{ fontSize: 10, color: 'var(--text-faint)' }}>{stepData.txTable.length} Transactions</span>
            </div>
            {stepData.txTable.length === 0 ? (
              <div style={{ padding: '12px', textAlign: 'center', fontSize: 11, color: 'var(--text-muted)', background: 'var(--surface-2)', borderRadius: 6 }}>
                Transaction table is empty (All memory states reset).
              </div>
            ) : (
              <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 11 }}>
                <thead>
                  <tr style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-muted)', textAlign: 'left' }}>
                    <th style={{ padding: '4px 6px' }}>Tx ID</th>
                    <th style={{ padding: '4px 6px' }}>Status</th>
                    <th style={{ padding: '4px 6px', textAlign: 'right' }}>LastLSN</th>
                  </tr>
                </thead>
                <tbody>
                  {stepData.txTable.map((tx) => (
                    <tr key={tx.id} style={{ borderBottom: '1px solid rgba(255,255,255,0.04)' }}>
                      <td style={{ padding: '6px', fontWeight: 700, fontFamily: 'var(--font-num)' }}>{tx.id}</td>
                      <td style={{ padding: '6px', color: tx.statusColor, fontWeight: 600 }}>{tx.status}</td>
                      <td style={{ padding: '6px', textAlign: 'right', fontFamily: 'var(--font-num)', color: 'var(--text-muted)' }}>
                        LSN #{tx.lastLsn}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            )}
          </div>

          {/* Dirty Page Table (DPT) */}
          <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 14 }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 8 }}>
              <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: 0.5 }}>
                Dirty Page Table (DPT)
              </span>
              <span style={{ fontSize: 10, color: 'var(--text-faint)' }}>Earliest Unflushed LSN</span>
            </div>
            {stepData.dirtyPageTable.length === 0 ? (
              <div style={{ padding: '12px', textAlign: 'center', fontSize: 11, color: 'var(--text-muted)', background: 'var(--surface-2)', borderRadius: 6 }}>
                DPT is empty (Disk state clean).
              </div>
            ) : (
              <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 11 }}>
                <thead>
                  <tr style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-muted)', textAlign: 'left' }}>
                    <th style={{ padding: '4px 6px' }}>Page ID</th>
                    <th style={{ padding: '4px 6px' }}>RecLSN</th>
                    <th style={{ padding: '4px 6px', textAlign: 'right' }}>State</th>
                  </tr>
                </thead>
                <tbody>
                  {stepData.dirtyPageTable.map((dpt) => (
                    <tr key={dpt.pageId} style={{ borderBottom: '1px solid rgba(255,255,255,0.04)' }}>
                      <td style={{ padding: '6px', fontWeight: 700, fontFamily: 'var(--font-num)' }}>{dpt.pageId}</td>
                      <td style={{ padding: '6px', fontFamily: 'var(--font-num)', color: 'var(--accent)' }}>
                        {dpt.recLsn > 0 ? `LSN #${dpt.recLsn}` : '0 (Clean)'}
                      </td>
                      <td style={{ padding: '6px', textAlign: 'right', fontWeight: 600, color: 'var(--text-muted)' }}>
                        {dpt.status}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            )}
          </div>
        </div>
      </div>
    </div>
  );
}


// ──────────────────────────────────────────────────────────────────────────────
// 4. EXTENDIBLE HASH INDEX VISUALIZER (Directory & Bucket Pointers)
// ──────────────────────────────────────────────────────────────────────────────

interface HashBucket {
  id: string;
  localDepth: number;
  slots: string[];
}

interface HashDirEntry {
  bits: string;
  bucket: string;
}

// Canonical hash codes guaranteeing rigorous prefix-to-bucket correspondence:
// - Prefix 00* -> Bucket B0
// - Prefix 010 -> Bucket B1 (D=3)
// - Prefix 011 -> Bucket B4 (D=3)
// - Prefix 01* -> Bucket B1 (D=2)
// - Prefix 10* -> Bucket B2
// - Prefix 11* -> Bucket B2 (d=1 in lookup) or Bucket B3 (d=2 after local split)
const KEY_HASH_MAP: Record<string, { hex: string; bin: string }> = {
  admin:             { hex: '0x0A4C', bin: '0000101001001100' }, // starts with 000
  omega_speedmaster: { hex: '0x32F1', bin: '0011001011110001' }, // starts with 001
  vinayak:           { hex: '0x48B2', bin: '0100100010110010' }, // starts with 010
  kanak:             { hex: '0x6C39', bin: '0110110000111001' }, // starts with 011
  gpu_91245:         { hex: '0x89D4', bin: '1000100111010100' }, // starts with 100
  antique_vase:      { hex: '0xC52A', bin: '1100010100101010' }, // starts with 110
  Vintage__19597:    { hex: '0xE471', bin: '1110010001110001' }, // starts with 111
  new_rare_coin:     { hex: '0x785E', bin: '0111100001011110' }, // starts with 011
};

const getKeyHash = (key: string) => {
  if (KEY_HASH_MAP[key]) return KEY_HASH_MAP[key];
  let hash = 0;
  for (let i = 0; i < key.length; i++) hash = (hash << 5) - hash + key.charCodeAt(i);
  const posHash = Math.abs(hash);
  return {
    hex: '0x' + (posHash % 0xFFFF).toString(16).toUpperCase().padStart(4, '0'),
    bin: (posHash % 0xFFFF).toString(2).padStart(16, '0'),
  };
};

function ExtendibleHashIndexVisualizer() {
  const [scenario, setScenario] = useState<'lookup' | 'insert_nosplit' | 'local_split' | 'dir_doubling'>('lookup');
  const [selectedKey, setSelectedKey] = useState('gpu_91245');

  // Compute state based on scenario
  const globalDepth = scenario === 'dir_doubling' ? 3 : 2;

  const directory: HashDirEntry[] = useMemo(() => {
    if (scenario === 'dir_doubling') {
      return [
        { bits: '000', bucket: 'B0' },
        { bits: '001', bucket: 'B0' },
        { bits: '010', bucket: 'B1' },
        { bits: '011', bucket: 'B4' },
        { bits: '100', bucket: 'B2' },
        { bits: '101', bucket: 'B2' },
        { bits: '110', bucket: 'B3' },
        { bits: '111', bucket: 'B3' },
      ];
    }
    if (scenario === 'local_split') {
      return [
        { bits: '00', bucket: 'B0' },
        { bits: '01', bucket: 'B1' },
        { bits: '10', bucket: 'B2' },
        { bits: '11', bucket: 'B3' },
      ];
    }
    // lookup & insert_nosplit
    return [
      { bits: '00', bucket: 'B0' },
      { bits: '01', bucket: 'B1' },
      { bits: '10', bucket: 'B2' },
      { bits: '11', bucket: 'B2' },
    ];
  }, [scenario]);

  const buckets: Record<string, HashBucket> = useMemo(() => {
    const res: Record<string, HashBucket> = {};
    if (scenario === 'dir_doubling') {
      res.B0 = { id: 'B0', localDepth: 2, slots: ['admin', 'omega_speedmaster'] };
      res.B1 = { id: 'B1', localDepth: 3, slots: ['vinayak'] };
      res.B4 = { id: 'B4', localDepth: 3, slots: ['kanak', 'new_rare_coin'] };
      res.B2 = { id: 'B2', localDepth: 2, slots: ['gpu_91245'] };
      res.B3 = { id: 'B3', localDepth: 2, slots: ['antique_vase', 'Vintage__19597'] };
    } else if (scenario === 'local_split') {
      res.B0 = { id: 'B0', localDepth: 2, slots: ['admin', 'omega_speedmaster'] };
      res.B1 = { id: 'B1', localDepth: 2, slots: ['vinayak', 'kanak'] };
      res.B2 = { id: 'B2', localDepth: 2, slots: ['gpu_91245'] };
      res.B3 = { id: 'B3', localDepth: 2, slots: ['antique_vase', 'Vintage__19597'] };
    } else {
      res.B0 = { id: 'B0', localDepth: 2, slots: ['admin', 'omega_speedmaster'] };
      res.B1 = { id: 'B1', localDepth: 2, slots: ['vinayak', 'kanak'] };
      res.B2 = { id: 'B2', localDepth: 1, slots: ['gpu_91245', 'antique_vase'] };
    }
    return res;
  }, [scenario]);

  // Keys available for quick selection in the current scenario
  const availableKeys = useMemo(() => {
    const base = ['vinayak', 'admin', 'kanak', 'gpu_91245', 'omega_speedmaster', 'antique_vase'];
    if (scenario === 'local_split') return [...base, 'Vintage__19597'];
    if (scenario === 'dir_doubling') return [...base, 'Vintage__19597', 'new_rare_coin'];
    return base;
  }, [scenario]);

  const handleScenarioChange = (newScenario: 'lookup' | 'insert_nosplit' | 'local_split' | 'dir_doubling') => {
    setScenario(newScenario);
    if (newScenario === 'local_split') {
      setSelectedKey('Vintage__19597');
    } else if (newScenario === 'dir_doubling') {
      setSelectedKey('new_rare_coin');
    } else if (newScenario === 'insert_nosplit') {
      setSelectedKey('antique_vase');
    } else {
      setSelectedKey('gpu_91245');
    }
  };

  // Deterministic hash calculation
  const lookupInfo = useMemo(() => {
    const { hex, bin } = getKeyHash(selectedKey);
    const bits = bin.slice(0, globalDepth);
    const matchDir = directory.find((d) => d.bits === bits) || directory[0];
    return {
      hashHex: hex,
      binStr: bin,
      prefixBits: bits,
      dirSlot: matchDir.bits,
      targetBucket: matchDir.bucket,
    };
  }, [selectedKey, globalDepth, directory]);

  // Vertical layout coordinates for SVG connecting curves
  // Directory on left (x=110), Buckets on right (x=410)
  const dirYMap: Record<string, number> = useMemo(() => {
    const map: Record<string, number> = {};
    const n = directory.length;
    const startY = 40;
    const stepY = n === 8 ? 32 : 55;
    directory.forEach((d, i) => {
      map[d.bits] = startY + i * stepY;
    });
    return map;
  }, [directory]);

  const bucketYMap: Record<string, number> = useMemo(() => {
    const keys = Object.keys(buckets);
    const map: Record<string, number> = {};
    const startY = 45;
    const stepY = 55;
    keys.forEach((bId, i) => {
      map[bId] = startY + i * stepY;
    });
    return map;
  }, [buckets]);

  return (
    <div className="card" style={{ padding: 24, marginBottom: 28 }}>
      {/* Title & Toolbar */}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', flexWrap: 'wrap', gap: 14, marginBottom: 16 }}>
        <div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
            <h2 className="section-title" style={{ margin: 0, fontSize: 17 }}>
              Extendible Hash Index · O(1) Secondary Lookup & Dynamic Splitting
            </h2>
            <span
              style={{
                fontSize: 10,
                fontWeight: 700,
                padding: '3px 9px',
                borderRadius: 12,
                background: 'rgba(230, 160, 30, 0.12)',
                color: 'var(--accent)',
                border: '1px solid var(--accent)',
                textTransform: 'uppercase',
                letterSpacing: 0.5,
              }}
            >
              GLOBAL DEPTH D={globalDepth}
            </span>
          </div>
          <p style={{ fontSize: 12, color: 'var(--text-muted)', margin: '4px 0 0 0' }}>
            Global directory of $2^D$ pointers routing hash prefix bits directly to physical disk pages. Buckets split dynamically without table-wide rehashing.
          </p>
        </div>

        {/* Scenario Switcher Buttons */}
        <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap' }}>
          <button
            className={`btn btn-sm ${scenario === 'lookup' ? 'btn-primary' : 'btn-ghost'}`}
            onClick={() => handleScenarioChange('lookup')}
            style={{ fontSize: 11, padding: '4px 10px', height: 28 }}
          >
            🔍 O(1) Lookup
          </button>
          <button
            className={`btn btn-sm ${scenario === 'insert_nosplit' ? 'btn-primary' : 'btn-ghost'}`}
            onClick={() => handleScenarioChange('insert_nosplit')}
            style={{ fontSize: 11, padding: '4px 10px', height: 28 }}
          >
            ➕ Insert (No Split)
          </button>
          <button
            className={`btn btn-sm ${scenario === 'local_split' ? 'btn-primary' : 'btn-ghost'}`}
            onClick={() => handleScenarioChange('local_split')}
            style={{ fontSize: 11, padding: '4px 10px', height: 28 }}
          >
            ⚡ Local Split (d &lt; D)
          </button>
          <button
            className={`btn btn-sm ${scenario === 'dir_doubling' ? 'btn-primary' : 'btn-ghost'}`}
            onClick={() => handleScenarioChange('dir_doubling')}
            style={{ fontSize: 11, padding: '4px 10px', height: 28 }}
          >
            💥 Directory Double (d = D)
          </button>
        </div>
      </div>

      {/* Interactive Key Selection Chips */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap', marginBottom: 18, background: 'var(--surface)', padding: '10px 14px', borderRadius: 8, border: '1px solid var(--border)' }}>
        <span style={{ fontSize: 12, fontWeight: 600, color: 'var(--text-muted)' }}>Query Key:</span>
        {availableKeys.map((k) => (
          <button
            key={k}
            className={`chip ${selectedKey === k ? 'active' : ''}`}
            onClick={() => setSelectedKey(k)}
            style={{ fontSize: 11, padding: '4px 10px', height: 26 }}
          >
            {k}
          </button>
        ))}
      </div>

      {/* Hash Bit Slicing Decomposition Bar */}
      <div style={{ background: 'var(--surface-2)', padding: '12px 16px', borderRadius: 8, border: '1px solid var(--border)', marginBottom: 18, fontSize: 12 }}>
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', flexWrap: 'wrap', gap: 12 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 14, flexWrap: 'wrap' }}>
            <span>Key: <strong style={{ color: 'var(--text)' }}>"{selectedKey}"</strong></span>
            <span>Hash: <code style={{ color: 'var(--accent)', fontFamily: 'var(--font-num)' }}>{lookupInfo.hashHex}</code></span>
            <span>
              16-Bit Binary: <code style={{ color: 'var(--text-muted)', fontFamily: 'var(--font-num)' }}>
                <span style={{ color: 'var(--accent)', fontWeight: 800, textDecoration: 'underline' }}>{lookupInfo.prefixBits}</span>
                {lookupInfo.binStr.slice(lookupInfo.prefixBits.length)}
              </code>
            </span>
          </div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
            <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>Prefix Resolution:</span>
            <span
              style={{
                background: 'rgba(16, 185, 129, 0.15)',
                color: 'var(--success)',
                border: '1px solid var(--success)',
                padding: '2px 8px',
                borderRadius: 4,
                fontFamily: 'var(--font-num)',
                fontWeight: 700,
                fontSize: 11,
              }}
            >
              Dir[{lookupInfo.dirSlot}] ──▶ Bucket {lookupInfo.targetBucket}
            </span>
          </div>
        </div>
      </div>

      {/* Main Grid: SVG Connecting Curves Left + Bucket Cards Right */}
      <div className="wfg-grid">
        {/* LEFT: SVG Connectors & Directory Slots */}
        <div className="hash-svg-wrap">
          <svg className="wfg-svg" viewBox="0 0 520 320">
            <defs>
              <marker id="hash-ah" markerWidth="8" markerHeight="6" refX="7" refY="3" orient="auto">
                <polygon points="0 0, 8 3, 0 6" fill="var(--border-strong)" />
              </marker>
              <marker id="hash-ah-active" markerWidth="8" markerHeight="6" refX="7" refY="3" orient="auto">
                <polygon points="0 0, 8 3, 0 6" fill="var(--accent)" />
              </marker>
              <filter id="hash-glow" x="-20%" y="-20%" width="140%" height="140%">
                <feGaussianBlur stdDeviation="3" result="blur" />
                <feComposite in="SourceGraphic" in2="blur" operator="over" />
              </filter>
            </defs>

            {/* Connecting Bezier Curves */}
            {directory.map((d) => {
              const y1 = dirYMap[d.bits] ?? 40;
              const y2 = bucketYMap[d.bucket] ?? 45;
              const isActive = lookupInfo.dirSlot === d.bits;

              return (
                <path
                  key={`curve-${d.bits}`}
                  d={`M 155 ${y1} C 270 ${y1}, 270 ${y2}, 385 ${y2}`}
                  className={`hash-curve ${isActive ? 'active' : ''}`}
                  markerEnd={isActive ? 'url(#hash-ah-active)' : 'url(#hash-ah)'}
                />
              );
            })}

            {/* Directory Slot Elements (Left Column) */}
            <text x="75" y="22" textAnchor="middle" fontSize="10" fontWeight="700" fill="var(--text-muted)" letterSpacing="0.05em">
              DIRECTORY (D={globalDepth})
            </text>

            {directory.map((d) => {
              const y = dirYMap[d.bits] ?? 40;
              const isActive = lookupInfo.dirSlot === d.bits;

              return (
                <g key={`dir-${d.bits}`} transform={`translate(75, ${y})`}>
                  <rect
                    x="-65"
                    y="-14"
                    width="140"
                    height="28"
                    rx="5"
                    fill={isActive ? 'var(--accent-wash)' : 'var(--surface)'}
                    stroke={isActive ? 'var(--accent)' : 'var(--border)'}
                    strokeWidth={isActive ? 2 : 1}
                    filter={isActive ? 'url(#hash-glow)' : undefined}
                  />
                  <text x="-48" y="4" fontSize="11" fontWeight="700" fontFamily="var(--font-num)" fill={isActive ? 'var(--accent)' : 'var(--text)'}>
                    [{d.bits}]
                  </text>
                  <text x="45" y="4" textAnchor="end" fontSize="10" fill="var(--text-muted)">
                    ➔ {d.bucket}
                  </text>
                </g>
              );
            })}

            {/* Bucket Node Anchors (Right Column) */}
            <text x="445" y="22" textAnchor="middle" fontSize="10" fontWeight="700" fill="var(--text-muted)" letterSpacing="0.05em">
              PHYSICAL BUCKETS
            </text>

            {Object.keys(buckets).map((bId) => {
              const y = bucketYMap[bId] ?? 45;
              const isTarget = lookupInfo.targetBucket === bId;

              return (
                <g key={`bucket-anchor-${bId}`} transform={`translate(445, ${y})`}>
                  <rect
                    x="-55"
                    y="-16"
                    width="110"
                    height="32"
                    rx="6"
                    fill={isTarget ? 'rgba(16, 185, 129, 0.15)' : 'var(--surface)'}
                    stroke={isTarget ? 'var(--success)' : 'var(--border)'}
                    strokeWidth={isTarget ? 2 : 1}
                  />
                  <text x="-40" y="4" fontSize="11" fontWeight="700" fill={isTarget ? 'var(--success)' : 'var(--text)'}>
                    {bId}
                  </text>
                  <text x="40" y="4" textAnchor="end" fontSize="9" fontFamily="var(--font-num)" fill="var(--text-muted)">
                    d={buckets[bId].localDepth}
                  </text>
                </g>
              );
            })}
          </svg>
        </div>

        {/* RIGHT: Bucket Data Pages & Slots List */}
        <div style={{ display: 'flex', flexDirection: 'column', gap: 14 }}>
          {/* Scenario Info Callout */}
          <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 14 }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 6 }}>
              <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--accent)', textTransform: 'uppercase', letterSpacing: 0.5 }}>
                {scenario === 'lookup'
                  ? 'O(1) Direct Key Lookup'
                  : scenario === 'insert_nosplit'
                  ? 'Insertion Without Split'
                  : scenario === 'local_split'
                  ? 'Local Split (d < D)'
                  : 'Directory Doubling (d = D)'}
              </span>
              <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>
                Target: Bucket {lookupInfo.targetBucket}
              </span>
            </div>
            <p style={{ fontSize: 12, color: 'var(--text-muted)', margin: 0, lineHeight: 1.5 }}>
              {scenario === 'lookup' &&
                `Key "${selectedKey}" hashes to ${lookupInfo.hashHex} with binary prefix [${lookupInfo.prefixBits}]. Index performs an O(1) memory lookup in directory slot Dir[${lookupInfo.dirSlot}], instantly yielding physical disk bucket ${lookupInfo.targetBucket}.`}
              {scenario === 'insert_nosplit' &&
                `Record "${selectedKey}" hashes to prefix [${lookupInfo.prefixBits}], routing via Dir[${lookupInfo.dirSlot}] to Bucket ${lookupInfo.targetBucket}. Because Bucket ${lookupInfo.targetBucket} has available slot space, the record is placed directly into the page without splitting or directory doubling.`}
              {scenario === 'local_split' &&
                `Bucket B2 previously overflowed at local depth d=1 while global depth D=2. Because d < D (1 < 2), Bucket B2 split locally into B2 (prefix [10]) and B3 (prefix [11]) with local depth d: 1 → 2. Directory size did NOT double! Only directory pointers [10] and [11] were remapped.`}
              {scenario === 'dir_doubling' &&
                `Bucket B1 with local depth d=2 overflowed when d = D=2! Because d equals global depth D, the directory doubled from 4 to 8 slots (2^2 → 2^3), global depth increased to D=3, and new bucket B4 was allocated (local depth d=3) to hold migrated key "kanak" and new key "new_rare_coin".`}
            </p>
          </div>

          {/* Physical Bucket Page Cards */}
          <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
            {Object.values(buckets).map((b) => {
              const isTarget = lookupInfo.targetBucket === b.id;

              return (
                <div
                  key={b.id}
                  style={{
                    border: `2px solid ${isTarget ? 'var(--success)' : 'var(--border)'}`,
                    background: isTarget ? 'rgba(16, 185, 129, 0.05)' : 'var(--surface)',
                    borderRadius: 8,
                    padding: 12,
                    transition: 'all 200ms ease',
                  }}
                >
                  <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 6 }}>
                    <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                      <span style={{ fontWeight: 700, fontSize: 13, color: isTarget ? 'var(--success)' : 'var(--text)' }}>
                        Bucket {b.id}
                      </span>
                      {isTarget && (
                        <span className="badge" style={{ fontSize: 9, background: 'rgba(16, 185, 129, 0.15)', color: 'var(--success)' }}>
                          RESOLVED TARGET
                        </span>
                      )}
                    </div>
                    <span style={{ fontSize: 11, color: 'var(--text-muted)', fontFamily: 'var(--font-num)' }}>
                      Local Depth d={b.localDepth} · {b.slots.length}/2 slots
                    </span>
                  </div>

                  <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap' }}>
                    {b.slots.map((s) => {
                      const cleanSlot = s.toLowerCase().split(' ')[0];
                      const isFoundKey = cleanSlot === selectedKey.toLowerCase() || s.toLowerCase() === selectedKey.toLowerCase();
                      return (
                        <div
                          key={s}
                          style={{
                            background: isFoundKey ? 'var(--accent-wash)' : 'var(--surface-2)',
                            border: `1px solid ${isFoundKey ? 'var(--accent)' : 'var(--border)'}`,
                            color: isFoundKey ? 'var(--accent)' : 'var(--text)',
                            borderRadius: 4,
                            padding: '3px 8px',
                            fontSize: 11,
                            fontFamily: 'var(--font-num)',
                            fontWeight: isFoundKey ? 700 : 500,
                          }}
                        >
                          {s} {isFoundKey && '✓'}
                        </div>
                      );
                    })}
                  </div>
                </div>
              );
            })}
          </div>
        </div>
      </div>
    </div>
  );
}

// ──────────────────────────────────────────────────────────────────────────────
// 5. DEADLOCK DETECTION · WAIT-FOR GRAPH (Interactive Cycle Abort)
// ──────────────────────────────────────────────────────────────────────────────

// ──────────────────────────────────────────────────────────────────────────────
// 5. DEADLOCK DETECTION · WAIT-FOR GRAPH (Interactive Cycle Abort & Recovery)
// ──────────────────────────────────────────────────────────────────────────────

interface WfgNodeState {
  stateClass: string;
  status: string;
  statusColor: string;
  held: string;
  wants: string;
}

interface WfgStepData {
  step: number;
  title: string;
  tag: string;
  tagColor: string;
  tagBg: string;
  desc: string;
  banner: string;
  bannerType: 'info' | 'warning' | 'danger' | 'success';
  nodes: {
    t1: WfgNodeState;
    t2: WfgNodeState;
    t3: WfgNodeState;
    t4: WfgNodeState;
  };
  edges: {
    e12: { active: boolean; cyc: boolean };
    e23: { active: boolean; cyc: boolean };
    e34: { active: boolean; cyc: boolean };
    e41: { active: boolean; cyc: boolean };
  };
  centerStatus: {
    title: string;
    sub: string;
    type: 'idle' | 'cycle' | 'severed' | 'resolved';
  };
  showCosts?: boolean;
}

const WFG_STEPS: WfgStepData[] = [
  {
    step: 0,
    title: 'Initial State — 4 Concurrent Bidders',
    tag: 'NORMAL 2PL',
    tagColor: 'var(--success)',
    tagBg: 'rgba(16, 185, 129, 0.12)',
    desc: 'Transactions T1..T4 concurrently acquire Exclusive (X) locks on Lots 1..4 under Strict 2-Phase Locking. All transactions are actively executing in the growing phase with zero lock contention.',
    banner: 'Strict 2PL Phase 1: All 4 transactions hold exclusive locks. No wait-for dependencies exist.',
    bannerType: 'info',
    nodes: {
      t1: { stateClass: 'run', status: 'RUNNING', statusColor: 'var(--success)', held: 'Lot 1 (Antique Clock)', wants: 'None' },
      t2: { stateClass: 'run', status: 'RUNNING', statusColor: 'var(--success)', held: 'Lot 2 (Vintage Camera)', wants: 'None' },
      t3: { stateClass: 'run', status: 'RUNNING', statusColor: 'var(--success)', held: 'Lot 3 (Rare Diamond)', wants: 'None' },
      t4: { stateClass: 'run', status: 'RUNNING', statusColor: 'var(--success)', held: 'Lot 4 (Painting)', wants: 'None' },
    },
    edges: {
      e12: { active: false, cyc: false },
      e23: { active: false, cyc: false },
      e34: { active: false, cyc: false },
      e41: { active: false, cyc: false },
    },
    centerStatus: { title: 'STRICT 2PL NORMAL', sub: '0 Wait Edges · All Running', type: 'idle' },
  },
  {
    step: 1,
    title: 'T1 Requests Lot 2 (Blocked by T2)',
    tag: 'LOCK CONTENTION',
    tagColor: '#f59e0b',
    tagBg: 'rgba(245, 158, 11, 0.12)',
    desc: 'Admin (T1) places a bid on Lot 2 to construct a multi-lot bundle. Because T2 already holds an Exclusive Lock on Lot 2, T1 cannot proceed. WFG adds directed dependency edge T1 → T2; T1 is suspended.',
    banner: 'Wait-For Edge Added: T1 → T2. T1 is blocked waiting for Lot 2 to be released.',
    bannerType: 'warning',
    nodes: {
      t1: { stateClass: 'blocked', status: 'BLOCKED', statusColor: '#f59e0b', held: 'Lot 1 (Antique Clock)', wants: 'Lot 2 (Camera)' },
      t2: { stateClass: 'run', status: 'RUNNING', statusColor: 'var(--success)', held: 'Lot 2 (Vintage Camera)', wants: 'None' },
      t3: { stateClass: 'run', status: 'RUNNING', statusColor: 'var(--success)', held: 'Lot 3 (Rare Diamond)', wants: 'None' },
      t4: { stateClass: 'run', status: 'RUNNING', statusColor: 'var(--success)', held: 'Lot 4 (Painting)', wants: 'None' },
    },
    edges: {
      e12: { active: true, cyc: false },
      e23: { active: false, cyc: false },
      e34: { active: false, cyc: false },
      e41: { active: false, cyc: false },
    },
    centerStatus: { title: 'T1 WAITS FOR T2', sub: 'Contention on Lot 2', type: 'idle' },
  },
  {
    step: 2,
    title: 'T2 Requests Lot 3 (Blocked by T3)',
    tag: 'DEPENDENCY CHAIN',
    tagColor: '#f59e0b',
    tagBg: 'rgba(245, 158, 11, 0.12)',
    desc: 'Bidder Kanak (T2) submits an outbid on Lot 3. Lot 3 is currently locked by T3. Edge T2 → T3 added to WFG. A linear dependency chain forms: T1 waits for T2, which waits for T3.',
    banner: 'Wait chain deepens: T1 → T2 → T3. Two transactions are now blocked.',
    bannerType: 'warning',
    nodes: {
      t1: { stateClass: 'blocked', status: 'BLOCKED', statusColor: '#f59e0b', held: 'Lot 1 (Antique Clock)', wants: 'Lot 2 (Camera)' },
      t2: { stateClass: 'blocked', status: 'BLOCKED', statusColor: '#f59e0b', held: 'Lot 2 (Vintage Camera)', wants: 'Lot 3 (Diamond)' },
      t3: { stateClass: 'run', status: 'RUNNING', statusColor: 'var(--success)', held: 'Lot 3 (Rare Diamond)', wants: 'None' },
      t4: { stateClass: 'run', status: 'RUNNING', statusColor: 'var(--success)', held: 'Lot 4 (Painting)', wants: 'None' },
    },
    edges: {
      e12: { active: true, cyc: false },
      e23: { active: true, cyc: false },
      e34: { active: false, cyc: false },
      e41: { active: false, cyc: false },
    },
    centerStatus: { title: 'DEPENDENCY CHAIN', sub: 'T1 → T2 → T3 (No Cycle)', type: 'idle' },
  },
  {
    step: 3,
    title: 'T3 Requests Lot 4 (Blocked by T4)',
    tag: 'DEPENDENCY CHAIN',
    tagColor: '#f59e0b',
    tagBg: 'rgba(245, 158, 11, 0.12)',
    desc: 'Bidder Vinayak (T3) attempts to acquire Lot 4. Lot 4 is held exclusively by T4. Edge T3 → T4 added. Dependency chain lengthens: T1 → T2 → T3 → T4. T4 is now the sole active transaction.',
    banner: 'Extended wait chain: T1 → T2 → T3 → T4. If T4 completes without needing Lot 1, no deadlock occurs.',
    bannerType: 'warning',
    nodes: {
      t1: { stateClass: 'blocked', status: 'BLOCKED', statusColor: '#f59e0b', held: 'Lot 1 (Antique Clock)', wants: 'Lot 2 (Camera)' },
      t2: { stateClass: 'blocked', status: 'BLOCKED', statusColor: '#f59e0b', held: 'Lot 2 (Vintage Camera)', wants: 'Lot 3 (Diamond)' },
      t3: { stateClass: 'blocked', status: 'BLOCKED', statusColor: '#f59e0b', held: 'Lot 3 (Rare Diamond)', wants: 'Lot 4 (Painting)' },
      t4: { stateClass: 'run', status: 'RUNNING', statusColor: 'var(--success)', held: 'Lot 4 (Painting)', wants: 'None' },
    },
    edges: {
      e12: { active: true, cyc: false },
      e23: { active: true, cyc: false },
      e34: { active: true, cyc: false },
      e41: { active: false, cyc: false },
    },
    centerStatus: { title: 'CHAIN: T1 → T2 → T3 → T4', sub: 'T4 is Root Holder', type: 'idle' },
  },
  {
    step: 4,
    title: 'T4 Requests Lot 1 — Deadlock Cycle Formed!',
    tag: 'DEADLOCK FORMED',
    tagColor: 'var(--danger)',
    tagBg: 'var(--danger-wash)',
    desc: 'Bidder4 (T4) requests Lot 1 held by T1. Edge T4 → T1 completes a directed cycle: T1 → T2 → T3 → T4 → T1! Every transaction is now blocked waiting for another in the cycle. Forward progress is impossible without external intervention.',
    banner: 'CRITICAL DEADLOCK: Directed cycle T1 → T2 → T3 → T4 → T1. System execution halted.',
    bannerType: 'danger',
    nodes: {
      t1: { stateClass: 'cyc', status: 'DEADLOCKED', statusColor: 'var(--danger)', held: 'Lot 1 (Antique Clock)', wants: 'Lot 2 (Camera)' },
      t2: { stateClass: 'cyc', status: 'DEADLOCKED', statusColor: 'var(--danger)', held: 'Lot 2 (Vintage Camera)', wants: 'Lot 3 (Diamond)' },
      t3: { stateClass: 'cyc', status: 'DEADLOCKED', statusColor: 'var(--danger)', held: 'Lot 3 (Rare Diamond)', wants: 'Lot 4 (Painting)' },
      t4: { stateClass: 'cyc', status: 'DEADLOCKED', statusColor: 'var(--danger)', held: 'Lot 4 (Painting)', wants: 'Lot 1 (Clock)' },
    },
    edges: {
      e12: { active: true, cyc: true },
      e23: { active: true, cyc: true },
      e34: { active: true, cyc: true },
      e41: { active: true, cyc: true },
    },
    centerStatus: { title: 'DEADLOCK CYCLE', sub: 'T1 → T2 → T3 → T4 → T1', type: 'cycle' },
  },
  {
    step: 5,
    title: 'Cycle Detection via Depth-First Search (DFS)',
    tag: 'GRAPH ALGORITHM',
    tagColor: 'var(--accent)',
    tagBg: 'var(--accent-wash)',
    desc: 'WALnut background Deadlock Detector wakes up on its 500ms epoch. It initiates a Depth-First Search (DFS) on the WFG. Tracking visited nodes and the active recursion stack, DFS traverses T1 → T2 → T3 → T4, and encounters T1 on the recursion stack! A back-edge confirms the cycle in O(V + E) time.',
    banner: 'DFS Graph Traversal: Recursion stack detects back-edge T4 → T1. Deadlock cycle confirmed.',
    bannerType: 'warning',
    nodes: {
      t1: { stateClass: 'visit', status: 'DFS ROOT', statusColor: 'var(--accent)', held: 'Lot 1 (Antique Clock)', wants: 'Lot 2 (Camera)' },
      t2: { stateClass: 'visit', status: 'VISITED', statusColor: 'var(--accent)', held: 'Lot 2 (Vintage Camera)', wants: 'Lot 3 (Diamond)' },
      t3: { stateClass: 'visit', status: 'VISITED', statusColor: 'var(--accent)', held: 'Lot 3 (Rare Diamond)', wants: 'Lot 4 (Painting)' },
      t4: { stateClass: 'visit', status: 'BACK-EDGE', statusColor: 'var(--accent)', held: 'Lot 4 (Painting)', wants: 'Lot 1 (Clock)' },
    },
    edges: {
      e12: { active: true, cyc: true },
      e23: { active: true, cyc: true },
      e34: { active: true, cyc: true },
      e41: { active: true, cyc: true },
    },
    centerStatus: { title: 'DFS CYCLE CONFIRMED', sub: 'Back-edge to T1 on Stack', type: 'cycle' },
  },
  {
    step: 6,
    title: 'Victim Selection: Min Rollback Cost Heuristic',
    tag: 'VICTIM SELECTION',
    tagColor: '#f59e0b',
    tagBg: 'rgba(245, 158, 11, 0.12)',
    desc: 'WALnut applies a Min-Rollback-Cost heuristic: Cost(Tx) = WAL_Records_Written + Locks_Held. Choosing the transaction with the fewest written log records minimizes ARIES undo logging and cascade overhead. T2 has written only 15 log records vs T1(40), T3(60), T4(25). T2 is designated as the victim.',
    banner: 'Victim Selected: T2 (Kanak) has the lowest rollback cost (15 WAL records). Abort scheduled.',
    bannerType: 'warning',
    nodes: {
      t1: { stateClass: 'blocked', status: 'SAFE (Cost 40)', statusColor: 'var(--text-muted)', held: 'Lot 1 (Antique Clock)', wants: 'Lot 2 (Camera)' },
      t2: { stateClass: 'cyc', status: 'SELECTED VICTIM', statusColor: 'var(--danger)', held: 'Lot 2 (Vintage Camera)', wants: 'Lot 3 (Diamond)' },
      t3: { stateClass: 'blocked', status: 'SAFE (Cost 60)', statusColor: 'var(--text-muted)', held: 'Lot 3 (Rare Diamond)', wants: 'Lot 4 (Painting)' },
      t4: { stateClass: 'blocked', status: 'SAFE (Cost 25)', statusColor: 'var(--text-muted)', held: 'Lot 4 (Painting)', wants: 'Lot 1 (Clock)' },
    },
    edges: {
      e12: { active: true, cyc: true },
      e23: { active: true, cyc: true },
      e34: { active: true, cyc: true },
      e41: { active: true, cyc: true },
    },
    centerStatus: { title: 'VICTIM: T2 (KANAK)', sub: 'Min Undo Cost (15 WAL recs)', type: 'cycle' },
    showCosts: true,
  },
  {
    step: 7,
    title: 'Abort & Rollback Victim (T2)',
    tag: 'ROLLBACK & UNDO',
    tagColor: 'var(--danger)',
    tagBg: 'var(--danger-wash)',
    desc: 'WALnut aborts victim T2. In the Write-Ahead Log, Compensation Log Records (CLRs) are written for each updated page. T2’s lock on Lot 2 is immediately released into the lock table, and its outbound dependency edge T2 → T3 is severed. The circular wait is broken!',
    banner: 'Victim T2 aborted! Lot 2 lock released into table. Circular wait-for dependency severed.',
    bannerType: 'danger',
    nodes: {
      t1: { stateClass: 'run', status: 'ACQUIRING LOT 2', statusColor: 'var(--success)', held: 'Lot 1 (Antique Clock)', wants: 'Lot 2 (Granted)' },
      t2: { stateClass: 'aborted shake', status: 'ABORTED (VICTIM)', statusColor: 'var(--danger)', held: 'Released', wants: 'Rollback Complete' },
      t3: { stateClass: 'blocked', status: 'BLOCKED', statusColor: '#f59e0b', held: 'Lot 3 (Rare Diamond)', wants: 'Lot 4 (Painting)' },
      t4: { stateClass: 'blocked', status: 'BLOCKED', statusColor: '#f59e0b', held: 'Lot 4 (Painting)', wants: 'Lot 1 (Clock)' },
    },
    edges: {
      e12: { active: false, cyc: false },
      e23: { active: false, cyc: false },
      e34: { active: true, cyc: false },
      e41: { active: true, cyc: false },
    },
    centerStatus: { title: 'CYCLE BROKEN', sub: 'T2 Aborted · Locks Released', type: 'severed' },
    showCosts: true,
  },
  {
    step: 8,
    title: 'T1 Acquires Lot 2, Commits & Releases Locks',
    tag: 'STRICT 2PL COMMIT',
    tagColor: 'var(--success)',
    tagBg: 'rgba(16, 185, 129, 0.12)',
    desc: 'With Lot 2 released by the victim, T1’s pending lock request is granted by the Lock Manager. T1 resumes execution, updates Lot 2 in the buffer pool, flushes a COMMIT log record to WAL, and releases all locks (Lot 1 & Lot 2) under Strict 2PL Phase 2.',
    banner: 'T1 executed and committed! Released Lot 1 and Lot 2. T4 can now acquire Lot 1.',
    bannerType: 'success',
    nodes: {
      t1: { stateClass: 'done', status: 'COMMITTED ✓', statusColor: 'var(--success)', held: 'Released', wants: 'Done' },
      t2: { stateClass: 'aborted', status: 'ABORTED', statusColor: 'var(--text-muted)', held: 'None', wants: 'Awaiting Restart' },
      t3: { stateClass: 'blocked', status: 'BLOCKED', statusColor: '#f59e0b', held: 'Lot 3 (Rare Diamond)', wants: 'Lot 4 (Painting)' },
      t4: { stateClass: 'run', status: 'ACQUIRING LOT 1', statusColor: 'var(--success)', held: 'Lot 4 (Painting)', wants: 'Lot 1 (Granted)' },
    },
    edges: {
      e12: { active: false, cyc: false },
      e23: { active: false, cyc: false },
      e34: { active: true, cyc: false },
      e41: { active: false, cyc: false },
    },
    centerStatus: { title: 'T1 COMMITTED', sub: 'Locks Released · T4 Unblocks', type: 'resolved' },
  },
  {
    step: 9,
    title: 'Cascade Completion & Automatic Transaction Restart',
    tag: 'ALL RECOVERED',
    tagColor: 'var(--success)',
    tagBg: 'rgba(16, 185, 129, 0.12)',
    desc: 'With Lot 1 free, T4 acquires it, executes and commits. T4 releases Lot 4, unblocking T3, which acquires Lot 4 and commits. Finally, aborted transaction T2 is automatically re-submitted by the client runtime and succeeds without conflict. Zero data loss, ACID consistency maintained!',
    banner: 'Full resolution achieved: T1, T3, T4 committed. Aborted T2 restarted safely. ACID preserved.',
    bannerType: 'success',
    nodes: {
      t1: { stateClass: 'done', status: 'COMMITTED ✓', statusColor: 'var(--success)', held: 'None', wants: 'Done' },
      t2: { stateClass: 'run', status: 'RESTARTED (OK)', statusColor: 'var(--success)', held: 'Lot 2 (Re-acquired)', wants: 'Committed ✓' },
      t3: { stateClass: 'done', status: 'COMMITTED ✓', statusColor: 'var(--success)', held: 'None', wants: 'Done' },
      t4: { stateClass: 'done', status: 'COMMITTED ✓', statusColor: 'var(--success)', held: 'None', wants: 'Done' },
    },
    edges: {
      e12: { active: false, cyc: false },
      e23: { active: false, cyc: false },
      e34: { active: false, cyc: false },
      e41: { active: false, cyc: false },
    },
    centerStatus: { title: 'ALL TRANSACTIONS COMMITTED', sub: 'Zero Data Loss · ACID Intact', type: 'resolved' },
  },
];

const TX_METRICS = [
  { id: 'T1', name: 'T1 (Admin)', role: 'Host / Admin', held: 'Lot 1', cost: 40, barPct: 66 },
  { id: 'T2', name: 'T2 (Kanak)', role: 'Bidder', held: 'Lot 2', cost: 15, barPct: 25, isVictim: true },
  { id: 'T3', name: 'T3 (Vinayak)', role: 'Bidder', held: 'Lot 3', cost: 60, barPct: 100 },
  { id: 'T4', name: 'T4 (Bidder4)', role: 'Bidder', held: 'Lot 4', cost: 25, barPct: 41 },
];

const DFS_CALL_STACK = [
  { frame: 'dfs_visit(T1)', role: 'Root', action: 'Push T1 onto recursion stack. Mark visited.', note: 'Explore out-edges from T1' },
  { frame: 'dfs_visit(T2)', role: 'T1 → T2', action: 'Traverse edge (T1, T2). Push T2 onto recursion stack.', note: 'Explore out-edges from T2' },
  { frame: 'dfs_visit(T3)', role: 'T2 → T3', action: 'Traverse edge (T2, T3). Push T3 onto recursion stack.', note: 'Explore out-edges from T3' },
  { frame: 'dfs_visit(T4)', role: 'T3 → T4', action: 'Traverse edge (T3, T4). Push T4 onto recursion stack.', note: 'Explore out-edges from T4' },
  { frame: 'detect_cycle(T1)', role: 'T4 → T1 (Back-Edge)', action: 'Neighbor T1 is already on current recursion stack!', note: '⚡ Directed cycle detected: T1→T2→T3→T4→T1' },
];

// Token interpolation coordinates along cycle for Step 5
const DFS_TOKEN_WAYPOINTS = [
  { x: 115, y: 85, label: 'T1' },
  { x: 265, y: 85, label: 'T1 → T2' },
  { x: 415, y: 85, label: 'T2' },
  { x: 415, y: 190, label: 'T2 → T3' },
  { x: 415, y: 295, label: 'T3' },
  { x: 265, y: 295, label: 'T3 → T4' },
  { x: 115, y: 295, label: 'T4' },
  { x: 115, y: 190, label: 'T4 → T1 (Back-edge)' },
];

function DeadlockWaitGraphVisualizer() {
  const [currentStep, setCurrentStep] = useState(0);
  const [isPlaying, setIsPlaying] = useState(false);
  const [dfsWaypointIndex, setDfsWaypointIndex] = useState(0);

  // Auto-play timeline loop
  useEffect(() => {
    if (!isPlaying) return;
    const interval = setInterval(() => {
      setCurrentStep((prev) => {
        if (prev >= WFG_STEPS.length - 1) {
          setIsPlaying(false);
          return prev;
        }
        return prev + 1;
      });
    }, 2400);
    return () => clearInterval(interval);
  }, [isPlaying]);

  // Animate DFS token around cycle when step === 5
  useEffect(() => {
    if (currentStep !== 5) return;
    const interval = setInterval(() => {
      setDfsWaypointIndex((prev) => (prev + 1) % DFS_TOKEN_WAYPOINTS.length);
    }, 600);
    return () => clearInterval(interval);
  }, [currentStep]);

  const stepData = WFG_STEPS[currentStep];

  function handlePrev() {
    setIsPlaying(false);
    setCurrentStep((prev) => Math.max(0, prev - 1));
  }

  function handleNext() {
    setIsPlaying(false);
    setCurrentStep((prev) => Math.min(WFG_STEPS.length - 1, prev + 1));
  }

  function handleReset() {
    setIsPlaying(false);
    setCurrentStep(0);
  }

  function handleTogglePlay() {
    if (currentStep >= WFG_STEPS.length - 1) {
      setCurrentStep(0);
      setIsPlaying(true);
    } else {
      setIsPlaying(!isPlaying);
    }
  }

  const tok = DFS_TOKEN_WAYPOINTS[dfsWaypointIndex];

  return (
    <div className="card" style={{ padding: 24, marginBottom: 28 }}>
      {/* Title & Top Toolbar */}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'flex-start', flexWrap: 'wrap', gap: 14, marginBottom: 16 }}>
        <div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
            <h2 className="section-title" style={{ margin: 0, fontSize: 17 }}>
              Deadlock Detection & Recovery · Wait-For Graph (WFG)
            </h2>
            <span
              style={{
                fontSize: 10,
                fontWeight: 700,
                padding: '3px 9px',
                borderRadius: 12,
                background: stepData.tagBg,
                color: stepData.tagColor,
                border: `1px solid ${stepData.tagColor}`,
                textTransform: 'uppercase',
                letterSpacing: 0.5,
              }}
            >
              {stepData.tag}
            </span>
          </div>
          <p style={{ fontSize: 12, color: 'var(--text-muted)', margin: '4px 0 0 0' }}>
            Interactive simulation of cycle formation, background DFS detection, min-cost victim selection, and ARIES rollback recovery under Strict 2PL.
          </p>
        </div>

        {/* Action Controls */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap' }}>
          <button
            className="btn btn-ghost btn-sm"
            onClick={handlePrev}
            disabled={currentStep === 0}
            style={{ fontSize: 12, padding: '6px 12px' }}
            title="Previous step"
          >
            ◀ Prev
          </button>
          <button
            className={`btn btn-sm ${isPlaying ? 'btn-danger' : 'btn-primary'}`}
            onClick={handleTogglePlay}
            style={{ fontSize: 12, padding: '6px 14px', minWidth: 90 }}
          >
            {isPlaying ? '⏸ Pause' : currentStep >= WFG_STEPS.length - 1 ? '↺ Replay' : '▶ Auto-Play'}
          </button>
          <button
            className="btn btn-ghost btn-sm"
            onClick={handleNext}
            disabled={currentStep === WFG_STEPS.length - 1}
            style={{ fontSize: 12, padding: '6px 12px' }}
            title="Next step"
          >
            Next ▶
          </button>
          <button
            className="btn btn-ghost btn-sm"
            onClick={handleReset}
            style={{ fontSize: 12, padding: '6px 10px' }}
            title="Reset to step 0"
          >
            ↺ Reset
          </button>
        </div>
      </div>

      {/* Stepper Dots & Quick Jumps */}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: 10, padding: '10px 14px', background: 'var(--surface-2)', borderRadius: 8, border: '1px solid var(--border)', marginBottom: 16 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 6, flexWrap: 'wrap' }}>
          <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--text-muted)', marginRight: 6 }}>
            STEP {currentStep + 1}/10:
          </span>
          <div className="wfg-dots">
            {WFG_STEPS.map((s, idx) => (
              <button
                key={idx}
                className={`wfg-dot ${currentStep === idx ? 'active' : ''}`}
                onClick={() => { setIsPlaying(false); setCurrentStep(idx); }}
                title={`Step ${idx + 1}: ${s.title}`}
                style={{
                  width: currentStep === idx ? 22 : 12,
                  height: 12,
                  borderRadius: 6,
                  transition: 'all 200ms ease',
                  background: currentStep === idx ? 'var(--accent)' : currentStep > idx ? 'var(--border-strong)' : 'rgba(255,255,255,0.1)',
                }}
              />
            ))}
          </div>
        </div>

        {/* Quick Scenario Shortcuts */}
        <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap' }}>
          <button
            className="btn btn-ghost btn-sm"
            onClick={() => { setIsPlaying(false); setCurrentStep(4); }}
            style={{ fontSize: 11, padding: '4px 8px', color: currentStep === 4 ? 'var(--danger)' : 'var(--text-muted)' }}
          >
            ⚡ Deadlock (Step 4)
          </button>
          <button
            className="btn btn-ghost btn-sm"
            onClick={() => { setIsPlaying(false); setCurrentStep(5); }}
            style={{ fontSize: 11, padding: '4px 8px', color: currentStep === 5 ? 'var(--accent)' : 'var(--text-muted)' }}
          >
            🔍 DFS Trace (Step 5)
          </button>
          <button
            className="btn btn-ghost btn-sm"
            onClick={() => { setIsPlaying(false); setCurrentStep(7); }}
            style={{ fontSize: 11, padding: '4px 8px', color: currentStep === 7 ? 'var(--danger)' : 'var(--text-muted)' }}
          >
            💥 Abort Victim (Step 7)
          </button>
          <button
            className="btn btn-ghost btn-sm"
            onClick={() => { setIsPlaying(false); setCurrentStep(9); }}
            style={{ fontSize: 11, padding: '4px 8px', color: currentStep === 9 ? 'var(--success)' : 'var(--text-muted)' }}
          >
            ✓ Recovered (Step 9)
          </button>
        </div>
      </div>

      {/* Main Interactive Grid: SVG Graph Left + Diagnostics Right */}
      <div className="wfg-grid">
        {/* LEFT: Wait-For Graph SVG */}
        <div className="wfg-svg-wrap">
          <svg className="wfg-svg" viewBox="0 0 530 380">
            <defs>
              {/* Arrowhead markers */}
              <marker id="wfg-ah-default" markerWidth="9" markerHeight="6" refX="8" refY="3" orient="auto">
                <polygon points="0 0, 9 3, 0 6" fill="var(--border-strong)" />
              </marker>
              <marker id="wfg-ah-danger" markerWidth="9" markerHeight="6" refX="8" refY="3" orient="auto">
                <polygon points="0 0, 9 3, 0 6" fill="var(--danger)" />
              </marker>
              <marker id="wfg-ah-success" markerWidth="9" markerHeight="6" refX="8" refY="3" orient="auto">
                <polygon points="0 0, 9 3, 0 6" fill="var(--success)" />
              </marker>
              <marker id="wfg-ah-accent" markerWidth="9" markerHeight="6" refX="8" refY="3" orient="auto">
                <polygon points="0 0, 9 3, 0 6" fill="var(--accent)" />
              </marker>

              {/* Glow filter for cycle highlighting */}
              <filter id="wfg-pulse-glow" x="-20%" y="-20%" width="140%" height="140%">
                <feGaussianBlur stdDeviation="3.5" result="blur" />
                <feComposite in="SourceGraphic" in2="blur" operator="over" />
              </filter>
            </defs>

            {/* Background Axis Guidelines */}
            <line x1="115" y1="85" x2="415" y2="85" stroke="rgba(255,255,255,0.04)" strokeDasharray="4 4" />
            <line x1="415" y1="85" x2="415" y2="295" stroke="rgba(255,255,255,0.04)" strokeDasharray="4 4" />
            <line x1="415" y1="295" x2="115" y2="295" stroke="rgba(255,255,255,0.04)" strokeDasharray="4 4" />
            <line x1="115" y1="295" x2="115" y2="85" stroke="rgba(255,255,255,0.04)" strokeDasharray="4 4" />

            {/* EDGES */}
            {/* Edge E12: T1 → T2 (top horizontal) */}
            {stepData.edges.e12.active && (
              <g className={`wfg-edge ${stepData.edges.e12.cyc ? 'cyc' : ''}`}>
                <line
                  x1="145"
                  y1="85"
                  x2="385"
                  y2="85"
                  markerEnd={stepData.edges.e12.cyc ? 'url(#wfg-ah-danger)' : 'url(#wfg-ah-default)'}
                />
                <text x="265" y="72">
                  waits (Lot 2)
                </text>
              </g>
            )}

            {/* Edge E23: T2 → T3 (right vertical) */}
            {stepData.edges.e23.active && (
              <g className={`wfg-edge ${stepData.edges.e23.cyc ? 'cyc' : ''}`}>
                <line
                  x1="415"
                  y1="115"
                  x2="415"
                  y2="265"
                  markerEnd={stepData.edges.e23.cyc ? 'url(#wfg-ah-danger)' : 'url(#wfg-ah-default)'}
                />
                <text x="466" y="190">
                  waits (Lot 3)
                </text>
              </g>
            )}

            {/* Edge E34: T3 → T4 (bottom horizontal) */}
            {stepData.edges.e34.active && (
              <g className={`wfg-edge ${stepData.edges.e34.cyc ? 'cyc' : ''}`}>
                <line
                  x1="385"
                  y1="295"
                  x2="145"
                  y2="295"
                  markerEnd={stepData.edges.e34.cyc ? 'url(#wfg-ah-danger)' : 'url(#wfg-ah-default)'}
                />
                <text x="265" y="318">
                  waits (Lot 4)
                </text>
              </g>
            )}

            {/* Edge E41: T4 → T1 (left vertical) */}
            {stepData.edges.e41.active && (
              <g className={`wfg-edge ${stepData.edges.e41.cyc ? 'cyc' : ''}`}>
                <line
                  x1="115"
                  y1="265"
                  x2="115"
                  y2="115"
                  markerEnd={stepData.edges.e41.cyc ? 'url(#wfg-ah-danger)' : 'url(#wfg-ah-default)'}
                />
                <text x="64" y="190">
                  waits (Lot 1)
                </text>
              </g>
            )}

            {/* CENTER STATUS CARD */}
            <g transform="translate(165, 160)">
              <rect
                x="0"
                y="0"
                width="200"
                height="60"
                rx="8"
                fill={
                  stepData.centerStatus.type === 'cycle'
                    ? 'rgba(239, 68, 68, 0.16)'
                    : stepData.centerStatus.type === 'severed'
                    ? 'rgba(245, 158, 11, 0.16)'
                    : stepData.centerStatus.type === 'resolved'
                    ? 'rgba(16, 185, 129, 0.16)'
                    : 'rgba(17, 26, 51, 0.85)'
                }
                stroke={
                  stepData.centerStatus.type === 'cycle'
                    ? 'var(--danger)'
                    : stepData.centerStatus.type === 'severed'
                    ? '#f59e0b'
                    : stepData.centerStatus.type === 'resolved'
                    ? 'var(--success)'
                    : 'var(--border)'
                }
                strokeWidth={stepData.centerStatus.type === 'cycle' ? 2 : 1}
                filter={stepData.centerStatus.type === 'cycle' ? 'url(#wfg-pulse-glow)' : undefined}
              />
              <text
                x="100"
                y="26"
                textAnchor="middle"
                fontSize="11"
                fontWeight="700"
                fontFamily="var(--font-num)"
                fill={
                  stepData.centerStatus.type === 'cycle'
                    ? 'var(--danger)'
                    : stepData.centerStatus.type === 'severed'
                    ? '#f59e0b'
                    : stepData.centerStatus.type === 'resolved'
                    ? 'var(--success)'
                    : 'var(--text-muted)'
                }
              >
                {stepData.centerStatus.title}
              </text>
              <text
                x="100"
                y="44"
                textAnchor="middle"
                fontSize="10"
                fill="var(--text-muted)"
              >
                {stepData.centerStatus.sub}
              </text>
            </g>

            {/* DFS TOKEN ANIMATION (Step 5) */}
            {currentStep === 5 && (
              <g transform={`translate(${tok.x}, ${tok.y})`}>
                <circle r="9" fill="var(--accent)" stroke="#ffffff" strokeWidth="2" filter="url(#wfg-pulse-glow)" />
                <rect x="-32" y="-26" width="64" height="16" rx="4" fill="rgba(11, 16, 32, 0.9)" stroke="var(--accent)" strokeWidth="1" />
                <text x="0" y="-14" textAnchor="middle" fontSize="9" fontWeight="700" fill="var(--accent)">
                  DFS TRACE
                </text>
              </g>
            )}

            {/* TRANSACTION NODES */}

            {/* Node T1 (115, 85) */}
            <g
              className={`wfg-node ${stepData.nodes.t1.stateClass}`}
              transform="translate(115, 85)"
              style={{ '--x': '115px', '--y': '85px' } as React.CSSProperties}
            >
              <circle cx="0" cy="0" r="28" />
              <text className="node-id" x="0" y="-3">T1</text>
              <text className="node-sub" x="0" y="12">Admin</text>
            </g>
            <text x="115" y="32" textAnchor="middle" fontSize="10" fontWeight="600" fill={stepData.nodes.t1.statusColor}>
              {stepData.nodes.t1.status}
            </text>
            <text x="115" y="45" textAnchor="middle" fontSize="9" fill="var(--text-muted)">
              Holds: Lot 1
            </text>

            {/* Node T2 (415, 85) */}
            <g
              className={`wfg-node ${stepData.nodes.t2.stateClass}`}
              transform="translate(415, 85)"
              style={{ '--x': '415px', '--y': '85px' } as React.CSSProperties}
            >
              <circle cx="0" cy="0" r="28" />
              <text className="node-id" x="0" y="-3">T2</text>
              <text className="node-sub" x="0" y="12">Kanak</text>
            </g>
            <text x="415" y="32" textAnchor="middle" fontSize="10" fontWeight="600" fill={stepData.nodes.t2.statusColor}>
              {stepData.nodes.t2.status}
            </text>
            <text x="415" y="45" textAnchor="middle" fontSize="9" fill="var(--text-muted)">
              {currentStep >= 7 ? 'Locks Released' : 'Holds: Lot 2'}
            </text>

            {/* Node T3 (415, 295) */}
            <g
              className={`wfg-node ${stepData.nodes.t3.stateClass}`}
              transform="translate(415, 295)"
              style={{ '--x': '415px', '--y': '295px' } as React.CSSProperties}
            >
              <circle cx="0" cy="0" r="28" />
              <text className="node-id" x="0" y="-3">T3</text>
              <text className="node-sub" x="0" y="12">Vinayak</text>
            </g>
            <text x="415" y="342" textAnchor="middle" fontSize="10" fontWeight="600" fill={stepData.nodes.t3.statusColor}>
              {stepData.nodes.t3.status}
            </text>
            <text x="415" y="355" textAnchor="middle" fontSize="9" fill="var(--text-muted)">
              {currentStep >= 9 ? 'Locks Released' : 'Holds: Lot 3'}
            </text>

            {/* Node T4 (115, 295) */}
            <g
              className={`wfg-node ${stepData.nodes.t4.stateClass}`}
              transform="translate(115, 295)"
              style={{ '--x': '115px', '--y': '295px' } as React.CSSProperties}
            >
              <circle cx="0" cy="0" r="28" />
              <text className="node-id" x="0" y="-3">T4</text>
              <text className="node-sub" x="0" y="12">Bidder4</text>
            </g>
            <text x="115" y="342" textAnchor="middle" fontSize="10" fontWeight="600" fill={stepData.nodes.t4.statusColor}>
              {stepData.nodes.t4.status}
            </text>
            <text x="115" y="355" textAnchor="middle" fontSize="9" fill="var(--text-muted)">
              {currentStep >= 9 ? 'Locks Released' : 'Holds: Lot 4'}
            </text>
          </svg>
        </div>

        {/* RIGHT: Diagnostics, DFS Call Stack, Victim Costs & Lock Table */}
        <div style={{ display: 'flex', flexDirection: 'column', gap: 14 }}>
          {/* Step Explanation Card */}
          <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 16 }}>
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 6 }}>
              <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--accent)', textTransform: 'uppercase', letterSpacing: 0.5 }}>
                Step {stepData.step + 1} of 10
              </span>
              <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>
                Detector Epoch: 500ms
              </span>
            </div>
            <h3 style={{ fontSize: 15, fontWeight: 700, margin: '0 0 8px 0', color: 'var(--text)' }}>
              {stepData.title}
            </h3>
            <p style={{ fontSize: 12, lineHeight: 1.55, color: 'var(--text-muted)', margin: 0 }}>
              {stepData.desc}
            </p>

            {/* Banner Callout */}
            <div
              style={{
                marginTop: 12,
                padding: '8px 12px',
                borderRadius: 6,
                fontSize: 12,
                fontWeight: 600,
                background:
                  stepData.bannerType === 'danger'
                    ? 'var(--danger-wash)'
                    : stepData.bannerType === 'warning'
                    ? 'rgba(245, 158, 11, 0.1)'
                    : stepData.bannerType === 'success'
                    ? 'rgba(16, 185, 129, 0.1)'
                    : 'var(--surface-2)',
                color:
                  stepData.bannerType === 'danger'
                    ? 'var(--danger)'
                    : stepData.bannerType === 'warning'
                    ? '#f59e0b'
                    : stepData.bannerType === 'success'
                    ? 'var(--success)'
                    : 'var(--accent)',
                borderLeft: `3px solid ${
                  stepData.bannerType === 'danger'
                    ? 'var(--danger)'
                    : stepData.bannerType === 'warning'
                    ? '#f59e0b'
                    : stepData.bannerType === 'success'
                    ? 'var(--success)'
                    : 'var(--accent)'
                }`,
              }}
            >
              {stepData.banner}
            </div>
          </div>

          {/* Conditional Component 1: DFS Call Stack (Shown during Step 5) */}
          {currentStep === 5 && (
            <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 14 }}>
              <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 10 }}>
                <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--accent)', textTransform: 'uppercase', letterSpacing: 0.5 }}>
                  DFS Traversal Recursion Stack
                </span>
                <span className="badge" style={{ fontSize: 10, background: 'var(--accent-wash)', color: 'var(--accent)' }}>
                  O(V + E) Back-Edge Check
                </span>
              </div>
              <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
                {DFS_CALL_STACK.map((frame, i) => (
                  <div
                    key={i}
                    style={{
                      display: 'flex',
                      alignItems: 'center',
                      justifyContent: 'space-between',
                      padding: '6px 10px',
                      borderRadius: 6,
                      background: i === 4 ? 'var(--danger-wash)' : 'var(--surface-2)',
                      border: `1px solid ${i === 4 ? 'var(--danger)' : 'var(--border)'}`,
                      fontSize: 11,
                    }}
                  >
                    <div>
                      <span style={{ fontWeight: 700, color: i === 4 ? 'var(--danger)' : 'var(--accent)', marginRight: 8 }}>
                        {frame.frame}
                      </span>
                      <span style={{ color: 'var(--text-muted)' }}>{frame.action}</span>
                    </div>
                    <span style={{ fontSize: 10, fontWeight: 600, color: i === 4 ? 'var(--danger)' : 'var(--text-faint)' }}>
                      {frame.role}
                    </span>
                  </div>
                ))}
              </div>
            </div>
          )}

          {/* Conditional Component 2: Victim Cost Comparison (Shown during Step 6 & 7) */}
          {stepData.showCosts && (
            <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 14 }}>
              <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 10 }}>
                <span style={{ fontSize: 11, fontWeight: 700, color: '#f59e0b', textTransform: 'uppercase', letterSpacing: 0.5 }}>
                  Rollback Cost Metric: WAL Undo Records
                </span>
                <span style={{ fontSize: 10, color: 'var(--text-muted)' }}>
                  Cost = Undo Records Written
                </span>
              </div>
              <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
                {TX_METRICS.map((tx) => (
                  <div
                    key={tx.id}
                    className={`wfg-cost-row ${tx.isVictim ? 'min' : ''}`}
                    style={{
                      padding: '6px 8px',
                      borderRadius: 6,
                      background: tx.isVictim ? 'var(--danger-wash)' : 'var(--surface-2)',
                      border: tx.isVictim ? '1px solid var(--danger)' : '1px solid transparent',
                    }}
                  >
                    <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: 11, marginBottom: 4 }}>
                      <span style={{ fontWeight: 600, color: tx.isVictim ? 'var(--danger)' : 'var(--text)' }}>
                        {tx.name} {tx.isVictim && '⚡ SELECTED VICTIM'}
                      </span>
                      <span style={{ fontFamily: 'var(--font-num)', color: tx.isVictim ? 'var(--danger)' : 'var(--text-muted)' }}>
                        {tx.cost} WAL logs ({tx.cost} undo records)
                      </span>
                    </div>
                    <div style={{ width: '100%', height: 6, background: 'rgba(255,255,255,0.06)', borderRadius: 3, overflow: 'hidden' }}>
                      <div
                        className="wfg-cost-bar"
                        style={{
                          width: `${tx.barPct}%`,
                          background: tx.isVictim ? 'var(--danger)' : '#3b82f6',
                        }}
                      />
                    </div>
                  </div>
                ))}
              </div>
              <div style={{ marginTop: 8, fontSize: 10, color: 'var(--text-faint)', fontStyle: 'italic' }}>
                * Min cost rule selects T2 (15 records) to minimize Compensation Log Records (CLRs) and recovery latency.
              </div>
            </div>
          )}

          {/* Live Lock & Transaction Table */}
          <div style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8, padding: 14 }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 8 }}>
              <span style={{ fontSize: 11, fontWeight: 700, color: 'var(--text-muted)', textTransform: 'uppercase', letterSpacing: 0.5 }}>
                Active Strict 2PL Lock Table
              </span>
              <span style={{ fontSize: 10, color: 'var(--text-faint)' }}>4 Transactions</span>
            </div>
            <table style={{ width: '100%', borderCollapse: 'collapse', fontSize: 11 }}>
              <thead>
                <tr style={{ borderBottom: '1px solid var(--border)', color: 'var(--text-muted)', textAlign: 'left' }}>
                  <th style={{ padding: '4px 6px' }}>Tx</th>
                  <th style={{ padding: '4px 6px' }}>Client</th>
                  <th style={{ padding: '4px 6px' }}>Exclusive Lock</th>
                  <th style={{ padding: '4px 6px' }}>Requested</th>
                  <th style={{ padding: '4px 6px', textAlign: 'right' }}>Status</th>
                </tr>
              </thead>
              <tbody>
                <tr style={{ borderBottom: '1px solid rgba(255,255,255,0.04)' }}>
                  <td style={{ padding: '6px', fontWeight: 700, fontFamily: 'var(--font-num)' }}>T1</td>
                  <td style={{ padding: '6px', color: 'var(--text-muted)' }}>Admin</td>
                  <td style={{ padding: '6px' }}>Lot 1 (Clock)</td>
                  <td style={{ padding: '6px', color: stepData.nodes.t1.wants !== 'None' ? '#f59e0b' : 'var(--text-faint)' }}>
                    {stepData.nodes.t1.wants}
                  </td>
                  <td style={{ padding: '6px', textAlign: 'right', fontWeight: 600, color: stepData.nodes.t1.statusColor }}>
                    {stepData.nodes.t1.status}
                  </td>
                </tr>
                <tr style={{ borderBottom: '1px solid rgba(255,255,255,0.04)' }}>
                  <td style={{ padding: '6px', fontWeight: 700, fontFamily: 'var(--font-num)' }}>T2</td>
                  <td style={{ padding: '6px', color: 'var(--text-muted)' }}>Kanak</td>
                  <td style={{ padding: '6px' }}>{currentStep >= 7 ? 'None (Released)' : 'Lot 2 (Camera)'}</td>
                  <td style={{ padding: '6px', color: stepData.nodes.t2.wants !== 'None' ? '#f59e0b' : 'var(--text-faint)' }}>
                    {stepData.nodes.t2.wants}
                  </td>
                  <td style={{ padding: '6px', textAlign: 'right', fontWeight: 600, color: stepData.nodes.t2.statusColor }}>
                    {stepData.nodes.t2.status}
                  </td>
                </tr>
                <tr style={{ borderBottom: '1px solid rgba(255,255,255,0.04)' }}>
                  <td style={{ padding: '6px', fontWeight: 700, fontFamily: 'var(--font-num)' }}>T3</td>
                  <td style={{ padding: '6px', color: 'var(--text-muted)' }}>Vinayak</td>
                  <td style={{ padding: '6px' }}>{currentStep >= 9 ? 'None (Released)' : 'Lot 3 (Diamond)'}</td>
                  <td style={{ padding: '6px', color: stepData.nodes.t3.wants !== 'None' ? '#f59e0b' : 'var(--text-faint)' }}>
                    {stepData.nodes.t3.wants}
                  </td>
                  <td style={{ padding: '6px', textAlign: 'right', fontWeight: 600, color: stepData.nodes.t3.statusColor }}>
                    {stepData.nodes.t3.status}
                  </td>
                </tr>
                <tr>
                  <td style={{ padding: '6px', fontWeight: 700, fontFamily: 'var(--font-num)' }}>T4</td>
                  <td style={{ padding: '6px', color: 'var(--text-muted)' }}>Bidder4</td>
                  <td style={{ padding: '6px' }}>{currentStep >= 9 ? 'None (Released)' : 'Lot 4 (Painting)'}</td>
                  <td style={{ padding: '6px', color: stepData.nodes.t4.wants !== 'None' ? '#f59e0b' : 'var(--text-faint)' }}>
                    {stepData.nodes.t4.wants}
                  </td>
                  <td style={{ padding: '6px', textAlign: 'right', fontWeight: 600, color: stepData.nodes.t4.statusColor }}>
                    {stepData.nodes.t4.status}
                  </td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </div>
    </div>
  );
}

// ──────────────────────────────────────────────────────────────────────────────
// MAIN ENGINE EXPLORER PAGE
// ──────────────────────────────────────────────────────────────────────────────

export function EngineExplorer() {
  const [stats, setStats]   = useState<Stats | null>(null);
  const [items, setItems]   = useState<RawLot[]>([]);
  const [error, setError]   = useState<string | null>(null);
  const { ledger, status }  = useStream();
  const { user }            = useSession();
  const navigate            = useNavigate();
  const isAdmin             = user?.role === 'ADMIN' || user?.username === 'admin';

  useEffect(() => {
    api.stats()
      .then(setStats)
      .catch(() => setError('Could not fetch engine stats.'));
    api.items().then(setItems).catch(() => {});

    const id = setInterval(() => {
      api.stats().then(setStats).catch(() => {});
      api.items().then(setItems).catch(() => {});
    }, 5000);
    return () => clearInterval(id);
  }, []);

  return (
    <div style={{ maxWidth: 1080, margin: '0 auto', paddingBottom: 60 }}>
      {/* Header */}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: 16, marginBottom: 24 }}>
        <div>
          <h1 className="display" style={{ fontSize: 24, margin: 0 }}>Engine Explorer</h1>
          <p style={{ color: 'var(--text-muted)', fontSize: 13, margin: '4px 0 0 0' }}>
            Interactive real-time visualization of WALnut's database internals, storage engine, and concurrency controls.
          </p>
        </div>
        <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
          <div
            style={{
              display: 'inline-flex',
              alignItems: 'center',
              gap: 6,
              padding: '5px 12px',
              borderRadius: 100,
              background: status === 'open' ? 'var(--accent-wash)' : 'var(--surface)',
              border: '1px solid var(--border)',
              fontSize: 12,
              color: status === 'open' ? 'var(--accent)' : 'var(--text-muted)',
              fontFamily: 'var(--font-num)',
            }}
          >
            <span style={{ width: 8, height: 8, borderRadius: '50%', background: status === 'open' ? 'var(--success)' : 'var(--text-faint)', display: 'inline-block' }} />
            Engine Server: {status.toUpperCase()}
          </div>
        </div>
      </div>

      {error && <p style={{ color: 'var(--danger)', fontSize: 13, marginBottom: 16 }}>{error}</p>}

      {/* Admin Notice Banner if Admin */}
      {isAdmin && (
        <div
          className="card"
          style={{
            padding: '14px 20px',
            display: 'flex',
            justifyContent: 'space-between',
            alignItems: 'center',
            background: 'var(--surface)',
            borderLeft: '4px solid var(--accent)',
            marginBottom: 24,
            flexWrap: 'wrap',
            gap: 12,
          }}
        >
          <div>
            <span style={{ fontWeight: 600, fontSize: 14 }}>Database Administration & Storage Reset</span>
            <span style={{ color: 'var(--text-muted)', fontSize: 12, marginLeft: 10 }}>
              User management and database purge controls are located on the dedicated admin page.
            </span>
          </div>
          <button className="btn btn-ghost btn-sm" onClick={() => navigate('/admin')} style={{ fontSize: 12 }}>
            🛡 Open Database Admin →
          </button>
        </div>
      )}

      {/* 1. Buffer Pool Clock Replacement Visualizer */}
      <ClockBufferPoolVisualizer stats={stats} />

      {/* 2. Live Ledger Component (High-Contrast Table with Filters) */}
      <LiveLedgerTable ledger={ledger} />

      {/* 3. Strict 2PL Bid Journey Visualizer */}
      <Strict2PLJourneyVisualizer />

      {/* 4. Write-Ahead Log & ARIES Recovery Simulator */}
      <AriesRecoverySimulator items={items} />

      {/* 5. Extendible Hash Index Visualizer */}
      <ExtendibleHashIndexVisualizer />

      {/* 6. Deadlock Wait-For Graph Visualizer */}
      <DeadlockWaitGraphVisualizer />
    </div>
  );
}
