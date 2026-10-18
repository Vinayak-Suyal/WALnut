// Host Console – comprehensive lot management for hosts and administrators.
// Supports creating lots, managing active lots, and inspecting previously listed items
// with winning bids, end timestamps, winner details, and date range segregation.

import { useState, useEffect, useCallback, type ReactNode } from 'react';
import { QRCodeSVG } from 'qrcode.react';
import { api } from '../lib/api';
import { useSession } from '../context/SessionContext';
import { useStreamEvents } from '../context/StreamContext';
import { useToast } from '../context/ToastContext';
import { normaliseLot } from '../types';
import type { Lot, Category, Condition, HostInfo, StreamEvent } from '../types';
import {
  Countdown, Money, EmptyState,
  labelCategory,
} from '../components/Primitives';
import { ConfirmModal } from '../components/ConfirmModal';

const CATEGORIES: Category[] = [
  'ELECTRONICS', 'WATCHES', 'ART', 'COLLECTIBLES', 'MUSIC', 'GENERAL',
];
const CONDITIONS: Condition[] = ['NEW', 'LIKE_NEW', 'GOOD', 'FAIR'];

type DateRange = 'all' | '24h' | '7d' | '30d' | '180d';

export function HostConsole() {
  const { user } = useSession();
  const toast = useToast();

  // Active clock tick every second to keep timers and active lots fresh
  const [clock, setClock] = useState(() => Date.now());
  useEffect(() => {
    const id = setInterval(() => setClock(Date.now()), 1000);
    return () => clearInterval(id);
  }, []);

  // Form state
  const [title, setTitle]             = useState('');
  const [category, setCategory]       = useState<Category>('ELECTRONICS');
  const [condition, setCondition]     = useState<Condition>('NEW');
  const [startPrice, setStartPrice]   = useState('');
  const [duration, setDuration]       = useState(1800); // 30 min default
  const [images, setImages]           = useState<File[]>([]);
  const [previews, setPreviews]       = useState<string[]>([]);
  const [publishing, setPublishing]   = useState(false);
  const [uploading, setUploading]     = useState(false);

  const [confirmModal, setConfirmModal] = useState<{
    open: boolean;
    title: string;
    message: ReactNode;
    confirmLabel?: string;
    variant?: 'danger' | 'warning' | 'primary';
    loading?: boolean;
    onConfirm: () => Promise<void> | void;
  }>({
    open: false,
    title: '',
    message: '',
    onConfirm: () => {},
  });

  // Managed lots
  const [myLots, setMyLots]           = useState<Lot[]>([]);
  const [lotsLoading, setLotsLoading] = useState(true);
  const [activeTab, setActiveTab]     = useState<'active' | 'past'>('active');
  const [dateRange, setDateRange]     = useState<DateRange>('all');

  // Host info (for QR)
  const [hostInfo, setHostInfo]       = useState<HostInfo | null>(null);
  const [copied, setCopied]           = useState(false);

  useEffect(() => {
    api.hostInfo().then(setHostInfo).catch(() => {});
  }, []);

  const isAdmin = user?.role === 'ADMIN' || user?.username === 'admin';
  const [viewAll, setViewAll] = useState(false);

  const fetchMyLots = useCallback(async () => {
    if (!user) return;
    try {
      setLotsLoading(true);
      const raw = (isAdmin && viewAll) ? await api.items() : await api.hostItems(user.user_id);
      setMyLots(raw.map(normaliseLot));
    } catch { /* silent */ }
    finally { setLotsLoading(false); }
  }, [user, isAdmin, viewAll]);

  useEffect(() => { fetchMyLots(); }, [fetchMyLots]);

  // Real-time synchronization for new bids, item status changes, anti-sniping timer extensions
  useStreamEvents(
    useCallback(
      (event: StreamEvent) => {
        if (
          event.type === 'new_bid' ||
          event.type === 'new_item' ||
          event.type === 'item_closed' ||
          event.type === 'item_deleted' ||
          event.type === 'db_reset'
        ) {
          fetchMyLots();
        }
      },
      [fetchMyLots],
    ),
  );

  function addFiles(files: FileList | File[]) {
    const arr = Array.from(files).slice(0, 3 - images.length);
    if (!arr.length) return;
    setImages((prev) => [...prev, ...arr].slice(0, 3));
    arr.forEach((f) => {
      const url = URL.createObjectURL(f);
      setPreviews((prev) => [...prev, url].slice(0, 3));
    });
  }

  function removeImage(i: number) {
    setImages((p) => p.filter((_, j) => j !== i));
    setPreviews((p) => { URL.revokeObjectURL(p[i]); return p.filter((_, j) => j !== i); });
  }

  async function publish() {
    if (!user) { toast('Please sign in first.', 'error'); return; }
    if (!title.trim()) { toast('Title is required.', 'error'); return; }
    const sp = parseFloat(startPrice);
    if (isNaN(sp) || sp <= 0) { toast('Starting price must be greater than 0.', 'error'); return; }

    setPublishing(true);
    try {
      let urls: string[] = [];
      if (images.length > 0) {
        setUploading(true);
        const uploadResults = await Promise.all(images.map((f) => api.upload(f)));
        urls = uploadResults;
        setUploading(false);
      }

      const end_time_ms = Date.now() + duration * 1000;

      await api.createItem({
        title: title.trim(),
        category,
        condition,
        starting_price: sp,
        end_time_ms,
        image_urls: urls,
        host_id: user.user_id,
      });

      toast('Lot published!', 'success');
      setTitle('');
      setStartPrice('');
      setImages([]);
      setPreviews([]);
      fetchMyLots();
    } catch (err: unknown) {
      toast(err instanceof Error ? err.message : 'Publish failed.', 'error');
    } finally {
      setPublishing(false);
      setUploading(false);
    }
  }

  function closeLot(id: string) {
    setConfirmModal({
      open: true,
      title: 'Close Auction Lot',
      variant: 'warning',
      confirmLabel: 'Close Lot',
      message: 'Close this lot? This finalises the auction under strict Two-Phase Locking (2PL) and declares the highest bidder winner.',
      onConfirm: async () => {
        try {
          await api.closeItem(id);
          toast('Lot closed.', 'success');
          setConfirmModal((p) => ({ ...p, open: false }));
          fetchMyLots();
        } catch (err: unknown) {
          toast(err instanceof Error ? err.message : 'Failed to close.', 'error');
        }
      },
    });
  }

  function cancelLot(id: string) {
    setConfirmModal({
      open: true,
      title: 'Cancel Auction Lot',
      variant: 'danger',
      confirmLabel: 'Cancel Lot',
      message: 'Cancel this lot? The auction will immediately terminate with no winner declared.',
      onConfirm: async () => {
        try {
          await api.cancelItem(id);
          toast('Lot cancelled.', 'info');
          setConfirmModal((p) => ({ ...p, open: false }));
          fetchMyLots();
        } catch (err: unknown) {
          toast(err instanceof Error ? err.message : 'Failed to cancel.', 'error');
        }
      },
    });
  }

  function deleteLot(id: string) {
    setConfirmModal({
      open: true,
      title: 'Delete Auction Lot',
      variant: 'danger',
      confirmLabel: 'Permanently Delete',
      message: 'Permanently delete this lot from the database? This will reclaim slotted page storage and cannot be undone.',
      onConfirm: async () => {
        try {
          await api.deleteItem(id);
          toast('Lot permanently deleted.', 'info');
          setConfirmModal((p) => ({ ...p, open: false }));
          fetchMyLots();
        } catch (err: unknown) {
          toast(err instanceof Error ? err.message : 'Failed to delete lot.', 'error');
        }
      },
    });
  }

  async function copyLink() {
    const url = hostInfo?.dashboard_url ?? window.location.origin;
    await navigator.clipboard.writeText(url);
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  }

  const qrUrl = hostInfo?.dashboard_url ?? window.location.origin;

  // Separate Active Lots vs Previously Listed Lots using live ticking clock
  const activeLots = myLots.filter((lot) => {
    const isTimeUp = lot.endsAt ? new Date(lot.endsAt).getTime() <= clock : false;
    return lot.status === 'OPEN' && !isTimeUp;
  });

  const pastLotsRaw = myLots.filter((lot) => {
    const isTimeUp = lot.endsAt ? new Date(lot.endsAt).getTime() <= clock : false;
    return lot.status !== 'OPEN' || isTimeUp;
  });

  // Filter past listings by selected date range
  const pastLots = pastLotsRaw.filter((lot) => {
    if (dateRange === 'all') return true;
    const endedMs = lot.endsAt ? new Date(lot.endsAt).getTime() : 0;
    const diffMs = clock - endedMs;
    if (dateRange === '24h') return diffMs <= 24 * 3600 * 1000;
    if (dateRange === '7d')  return diffMs <= 7 * 24 * 3600 * 1000;
    if (dateRange === '30d') return diffMs <= 30 * 24 * 3600 * 1000;
    if (dateRange === '180d') return diffMs <= 180 * 24 * 3600 * 1000;
    return true;
  });

  return (
    <>
      <ConfirmModal
        open={confirmModal.open}
        title={confirmModal.title}
        message={confirmModal.message}
        confirmLabel={confirmModal.confirmLabel}
        variant={confirmModal.variant}
        loading={confirmModal.loading}
        onConfirm={confirmModal.onConfirm}
        onCancel={() => setConfirmModal((prev) => ({ ...prev, open: false }))}
      />
      {/* Host Operations Header */}
      <div style={{ marginBottom: 20 }}>
        <h1 style={{
          fontFamily: 'var(--font-display)',
          fontSize: 24,
          fontWeight: 700,
          letterSpacing: '-0.02em',
          color: 'var(--text)',
          margin: 0,
        }}>
          Host Console
        </h1>
      </div>

      <div className="host-grid">
        {/* Left – publish form */}
        <div className="card host-form-card">
          <div style={{ marginBottom: 16 }}>
            <h2 className="section-title" style={{ margin: 0, fontSize: 16, fontWeight: 600 }}>
              Publish new lot
            </h2>
          </div>

          <div className="field">
            <label className="field-label" htmlFor="lot-title">
              <span>Item Title (Max 95 Chars)</span>
              <span className="char-count">{title.length}/95</span>
            </label>
            <input
              id="lot-title"
              className="field-input"
              maxLength={95}
              placeholder="e.g. 1968 Omega Speedmaster Professional"
              value={title}
              onChange={(e) => setTitle(e.target.value)}
              disabled={publishing}
            />
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 14 }}>
            <div className="field">
              <label className="field-label" htmlFor="lot-cat">Product Category</label>
              <select
                id="lot-cat"
                className="field-input"
                value={category}
                onChange={(e) => setCategory(e.target.value as Category)}
                disabled={publishing}
              >
                {CATEGORIES.map((c) => (
                  <option key={c} value={c}>{labelCategory(c)}</option>
                ))}
              </select>
            </div>

            <div className="field">
              <label className="field-label" htmlFor="lot-cond">Condition Grading</label>
              <select
                id="lot-cond"
                className="field-input"
                value={condition}
                onChange={(e) => setCondition(e.target.value as Condition)}
                disabled={publishing}
              >
                {CONDITIONS.map((c) => (
                  <option key={c} value={c}>{c.replace('_', ' ')}</option>
                ))}
              </select>
            </div>
          </div>

          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 14 }}>
            <div className="field">
              <label className="field-label" htmlFor="lot-sp">Starting Threshold ($ USD)</label>
              <input
                id="lot-sp"
                className="field-input"
                type="number"
                min="1"
                placeholder="50"
                value={startPrice}
                onChange={(e) => setStartPrice(e.target.value)}
                disabled={publishing}
              />
            </div>

            <div className="field">
              <label className="field-label" htmlFor="lot-dur">Auction Duration</label>
              <select
                id="lot-dur"
                className="field-input"
                value={duration}
                onChange={(e) => setDuration(Number(e.target.value))}
                disabled={publishing}
              >
                <option value={60}>1 minute (test)</option>
                <option value={300}>5 minutes</option>
                <option value={900}>15 minutes</option>
                <option value={1800}>30 minutes</option>
                <option value={3600}>1 hour</option>
                <option value={7200}>2 hours (Default)</option>
                <option value={86400}>24 hours</option>
              </select>
            </div>
          </div>

          {/* Photos */}
          <div className="field">
            <div className="field-label">
              <span>Asset Photographs ({images.length}/3 Uploaded)</span>
              <span style={{ fontSize: 10, color: 'var(--text-faint)' }}>Stored in ./data/uploads</span>
            </div>
            {/* Hidden native inputs: library (any image) and camera capture */}
            <input
              id="file-input-library"
              type="file"
              multiple
              accept="image/*"
              style={{ display: 'none' }}
              onChange={(e) => {
                if (e.target.files) addFiles(e.target.files);
                e.target.value = '';
              }}
              disabled={publishing || images.length >= 3}
            />
            <input
              id="file-input-camera"
              type="file"
              accept="image/*"
              capture="environment"
              style={{ display: 'none' }}
              onChange={(e) => {
                if (e.target.files) addFiles(e.target.files);
                e.target.value = '';
              }}
              disabled={publishing || images.length >= 3}
            />

            <div
              className="dropzone"
              onDragOver={(e) => e.preventDefault()}
              onDrop={(e) => { e.preventDefault(); addFiles(e.dataTransfer.files); }}
              onClick={() => document.getElementById('file-input-library')?.click()}
            >
              <div style={{ fontSize: 24, marginBottom: 6 }}>📷</div>
              <p style={{ fontSize: 13, fontWeight: 500, color: 'var(--text)', margin: '0 0 2px 0' }}>
                {images.length >= 3
                  ? 'Maximum 3 photos reached'
                  : 'Drag & drop photos here, or click to upload'}
              </p>
              <p style={{ fontSize: 11, color: 'var(--text-muted)', margin: '0 0 12px 0' }}>
                JPEG, PNG, WebP or GIF · max 5 MB each
              </p>

              {images.length < 3 && (
                <div
                  style={{ display: 'flex', gap: 10, flexWrap: 'wrap', justifyContent: 'center' }}
                  onClick={(e) => e.stopPropagation()}
                >
                  <button
                    type="button"
                    className="btn btn-subtle btn-sm"
                    style={{ fontSize: 12, display: 'flex', alignItems: 'center', gap: 6, height: 32 }}
                    onClick={() => document.getElementById('file-input-camera')?.click()}
                    disabled={publishing}
                  >
                    <span>📸</span> Take Photo (Camera)
                  </button>
                  <button
                    type="button"
                    className="btn btn-ghost btn-sm"
                    style={{ fontSize: 12, display: 'flex', alignItems: 'center', gap: 6, height: 32 }}
                    onClick={() => document.getElementById('file-input-library')?.click()}
                    disabled={publishing}
                  >
                    <span>📁</span> Choose from Photos
                  </button>
                </div>
              )}
            </div>

            {previews.length > 0 && (
              <div className="preview-strip">
                {previews.map((src, i) => (
                  <div key={src} className="preview-thumb">
                    <img src={src} alt={`Preview ${i + 1}`} />
                    <button
                      className="preview-remove"
                      onClick={(e) => { e.stopPropagation(); removeImage(i); }}
                      aria-label={`Remove photo ${i + 1}`}
                      type="button"
                    >
                      ✕
                    </button>
                  </div>
                ))}
              </div>
            )}
          </div>

          <button
            className="btn btn-primary btn-full"
            onClick={publish}
            disabled={publishing}
            style={{ marginTop: 10, height: 44, fontWeight: 600 }}
          >
            {uploading ? 'Uploading photos…' : publishing ? 'Publishing…' : '+ Publish Lot to The Floor'}
          </button>
        </div>

        {/* Right – LAN broadcast / QR */}
        <div className="host-side-col">
          <div className="card qr-card" style={{ display: 'flex', flexDirection: 'column' }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 16 }}>
              <span style={{ color: 'var(--accent)', fontSize: 16 }}>📱</span>
              <h2 className="section-title" style={{ margin: 0, fontSize: 16, fontWeight: 600 }}>
                Audience Mobile Pairing
              </h2>
            </div>

            <div className="qr-wrapper">
              <QRCodeSVG
                value={qrUrl}
                size={200}
                bgColor="#FFFFFF"
                fgColor="#101216"
                level="H"
                includeMargin={false}
                style={{ display: 'block' }}
              />
            </div>

            <div style={{ marginTop: 6 }}>
              <div style={{
                fontFamily: 'var(--font-num)',
                fontSize: 10,
                fontWeight: 600,
                letterSpacing: '0.08em',
                textTransform: 'uppercase',
                color: 'var(--text-muted)',
                marginBottom: 6,
              }}>
                Local Subnet Endpoint
              </div>
              <div style={{
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'space-between',
                background: 'var(--surface-2)',
                border: '1px solid var(--border)',
                borderRadius: 'var(--r-md)',
                padding: '6px 10px',
                gap: 8,
              }}>
                <span style={{
                  fontFamily: 'var(--font-num)',
                  fontSize: 12,
                  color: 'var(--accent)',
                  fontWeight: 600,
                  overflow: 'hidden',
                  textOverflow: 'ellipsis',
                  whiteSpace: 'nowrap',
                }}>
                  {qrUrl}
                </span>
                <button
                  type="button"
                  className="btn btn-sm btn-ghost"
                  onClick={copyLink}
                  style={{
                    padding: '4px 10px',
                    height: 28,
                    fontSize: 11,
                    fontFamily: 'var(--font-num)',
                    borderColor: 'var(--border-strong)',
                    flexShrink: 0,
                  }}
                >
                  {copied ? '✓ Copied' : 'Copy'}
                </button>
              </div>
            </div>
          </div>
        </div>
      </div>

      {/* Managed lots section with tabs */}
      <div className="managed-lots-section" style={{ marginTop: 28 }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: 12, marginBottom: 16 }}>
          {/* Tab selector */}
          <div style={{ display: 'flex', gap: 8, background: 'var(--surface-2)', padding: 4, borderRadius: 8 }}>
            <button
              className={`btn btn-sm ${activeTab === 'active' ? 'btn-primary' : 'btn-ghost'}`}
              onClick={() => setActiveTab('active')}
              style={{ fontSize: 13, padding: '6px 14px' }}
            >
              🟢 Live active lots ({activeLots.length})
            </button>
            <button
              className={`btn btn-sm ${activeTab === 'past' ? 'btn-primary' : 'btn-ghost'}`}
              onClick={() => setActiveTab('past')}
              style={{ fontSize: 13, padding: '6px 14px' }}
            >
              📦 Previously listed items ({pastLotsRaw.length})
            </button>
          </div>

          <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
            {activeTab === 'past' && (
              <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>Date range:</span>
                <select
                  className="field-input"
                  value={dateRange}
                  onChange={(e) => setDateRange(e.target.value as DateRange)}
                  style={{ fontSize: 12, padding: '0 28px 0 10px', height: 32, width: 'auto' }}
                >
                  <option value="all">All time</option>
                  <option value="24h">Past 24 hours</option>
                  <option value="7d">Past 7 days</option>
                  <option value="30d">Past 1 month (30 days)</option>
                  <option value="180d">Past 6 months</option>
                </select>
              </div>
            )}

            {isAdmin && (
              <button
                className="btn btn-ghost btn-sm"
                onClick={() => setViewAll((v) => !v)}
                style={{ fontSize: 12 }}
              >
                {viewAll ? 'Showing all lots (Admin)' : '🛡 View all lots'}
              </button>
            )}
          </div>
        </div>

        {lotsLoading ? (
          <p style={{ color: 'var(--text-muted)', fontSize: 13 }}>Loading lots from storage engine…</p>
        ) : activeTab === 'active' ? (
          /* ACTIVE LOTS TABLE */
          activeLots.length === 0 ? (
            <EmptyState
              title="No live active lots"
              sub="Publish a new lot above to start receiving live bids."
            />
          ) : (
            <div style={{ overflowX: 'auto', background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8 }}>
              <table className="lots-table">
                <thead>
                  <tr>
                    <th>Lot</th>
                    <th>Total Bids</th>
                    <th>Leading Bidder</th>
                    <th>Current Bid</th>
                    <th>Remaining Time</th>
                    <th>Status</th>
                    <th style={{ textAlign: 'right' }}>Actions</th>
                  </tr>
                </thead>
                <tbody>
                  {activeLots.map((lot) => (
                    <tr key={lot.id}>
                      <td style={{ maxWidth: 220 }}>
                        <div style={{ fontWeight: 600, fontSize: 13, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                          {lot.title}
                        </div>
                        <div style={{ fontSize: 11, color: 'var(--text-muted)', marginTop: 2 }}>
                          {labelCategory(lot.category)} · Host: {lot.host}
                        </div>
                      </td>
                      <td className="num" style={{ textAlign: 'center' }}>{lot.totalBids}</td>
                      <td style={{ fontSize: 12 }}>
                        {lot.currentWinner ? (
                          <span className="badge" style={{ background: 'var(--accent-wash)', color: 'var(--accent)' }}>
                            {lot.currentWinner}
                          </span>
                        ) : '—'}
                      </td>
                      <td>
                        {lot.currentBid != null
                          ? <Money value={lot.currentBid} />
                          : <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>
                              <Money value={lot.startingPrice} /> start
                            </span>
                        }
                      </td>
                      <td>
                        <Countdown endsAt={lot.endsAt} />
                      </td>
                      <td>
                        <span className="badge live">
                          <span className="live-dot" aria-hidden />
                          Live
                        </span>
                      </td>
                      <td style={{ textAlign: 'right' }}>
                        <div style={{ display: 'flex', gap: 6, justifyContent: 'flex-end' }}>
                          <button
                            className="btn btn-ghost btn-sm"
                            onClick={() => closeLot(lot.id)}
                            title="Close auction immediately"
                          >
                            Close
                          </button>
                          <button
                            className="btn btn-ghost btn-sm"
                            style={{ color: 'var(--warning)' }}
                            onClick={() => cancelLot(lot.id)}
                            title="Cancel lot"
                          >
                            Cancel
                          </button>
                          <button
                            className="btn btn-danger btn-sm"
                            onClick={() => deleteLot(lot.id)}
                            title="Permanently remove from database"
                          >
                            Delete
                          </button>
                        </div>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )
        ) : (
          /* PREVIOUSLY LISTED ITEMS (PAST / SETTLED LOTS) */
          pastLots.length === 0 ? (
            <EmptyState
              title="No previously listed items in selected range"
              sub={dateRange === 'all' ? 'Settled and ended items will appear here.' : 'Try changing the date range filter above.'}
            />
          ) : (
            <div style={{ overflowX: 'auto', background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 8 }}>
              <table className="lots-table">
                <thead>
                  <tr>
                    <th>Previously Listed Lot</th>
                    <th>Winning Bidder</th>
                    <th>Winning / Highest Bid</th>
                    <th>Ended Timestamp</th>
                    <th>Final Status</th>
                    <th style={{ textAlign: 'right' }}>Actions</th>
                  </tr>
                </thead>
                <tbody>
                  {pastLots.map((lot) => {
                    const endedDate = lot.endsAt ? new Date(lot.endsAt) : null;
                    const hasWinner = Boolean(lot.currentWinner);
                    return (
                      <tr key={lot.id}>
                        <td style={{ maxWidth: 220 }}>
                          <div style={{ fontWeight: 600, fontSize: 13, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                            {lot.title}
                          </div>
                          <div style={{ fontSize: 11, color: 'var(--text-muted)', marginTop: 2 }}>
                            {labelCategory(lot.category)} · Host: {lot.host}
                          </div>
                        </td>
                        <td style={{ fontSize: 12 }}>
                          {hasWinner ? (
                            <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                              <span style={{ fontSize: 13 }}>🏆</span>
                              <strong style={{ color: 'var(--text)' }}>{lot.currentWinner}</strong>
                            </div>
                          ) : (
                            <span style={{ color: 'var(--text-muted)' }}>No bids placed (Unsold)</span>
                          )}
                        </td>
                        <td>
                          {lot.currentBid != null && lot.currentBid > 0 ? (
                            <span style={{ fontWeight: 600, color: '#4ade80' }}>
                              <Money value={lot.currentBid} />
                            </span>
                          ) : (
                            <span style={{ color: 'var(--text-muted)', fontSize: 12 }}>
                              <Money value={lot.startingPrice} /> (reserve not met)
                            </span>
                          )}
                        </td>
                        <td style={{ fontSize: 12, color: 'var(--text-muted)', fontFamily: 'var(--font-num)' }}>
                          {endedDate ? endedDate.toLocaleString([], {
                            month: 'short', day: 'numeric', year: 'numeric',
                            hour: '2-digit', minute: '2-digit',
                          }) : '—'}
                        </td>
                        <td>
                          <span
                            className="badge"
                            style={
                              hasWinner
                                ? { color: '#4ade80', borderColor: 'rgba(74, 222, 128, 0.3)', background: 'rgba(74, 222, 128, 0.1)' }
                                : { color: 'var(--text-muted)', borderColor: 'var(--border)', background: 'var(--surface-2)' }
                            }
                          >
                            {hasWinner ? '✓ Sold' : lot.status === 'CANCELLED' ? 'Cancelled' : 'Unsold'}
                          </span>
                        </td>
                        <td style={{ textAlign: 'right' }}>
                          <button
                            className="btn btn-danger btn-sm"
                            onClick={() => deleteLot(lot.id)}
                            title="Permanently remove from database"
                          >
                            Delete record
                          </button>
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          )
        )}
      </div>
    </>
  );
}
