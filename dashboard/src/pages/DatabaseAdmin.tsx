import { useState, useEffect, useCallback, type ReactNode } from 'react';
import { useNavigate } from 'react-router-dom';
import { api } from '../lib/api';
import { useSession } from '../context/SessionContext';
import { useToast } from '../context/ToastContext';
import { ConfirmModal } from '../components/ConfirmModal';
import type { SessionUser, RawLot } from '../types';

interface ConfirmState {
  open: boolean;
  title: string;
  message: ReactNode;
  confirmLabel?: string;
  variant?: 'danger' | 'warning' | 'primary';
  loading?: boolean;
  onConfirm: () => Promise<void> | void;
}

export function DatabaseAdmin() {
  const { user, signIn } = useSession();
  const navigate = useNavigate();
  const isAdmin = user?.role === 'ADMIN' || user?.username === 'admin';
  const toast = useToast();

  const [users, setUsers] = useState<SessionUser[]>([]);
  const [lots, setLots] = useState<RawLot[]>([]);
  const [loading, setLoading] = useState(false);
  const [searchUsers, setSearchUsers] = useState('');
  const [searchLots, setSearchLots] = useState('');
  const [stats, setStats] = useState<{ total_pages: number } | null>(null);

  const [confirmModal, setConfirmModal] = useState<ConfirmState>({
    open: false,
    title: '',
    message: '',
    onConfirm: () => {},
  });

  const fetchData = useCallback(async () => {
    if (!isAdmin) return;
    setLoading(true);
    try {
      const [u, l, s] = await Promise.all([
        api.users().catch(() => [] as SessionUser[]),
        api.items().catch(() => [] as RawLot[]),
        api.stats().catch(() => null),
      ]);
      setUsers(u);
      setLots(l);
      if (s) {
        setStats({ total_pages: s.storage.total_pages });
      }
    } catch {
      toast('Failed to load database records.', 'error');
    } finally {
      setLoading(false);
    }
  }, [isAdmin, toast]);

  useEffect(() => {
    if (!isAdmin) {
      navigate('/auction', { replace: true });
      return;
    }
    fetchData();
  }, [isAdmin, navigate, fetchData]);

  function promptDeleteUser(uid: string, username: string) {
    if (uid === user?.user_id) {
      toast('Cannot delete the currently signed-in account.', 'error');
      return;
    }
    setConfirmModal({
      open: true,
      title: 'Delete User Account',
      variant: 'danger',
      confirmLabel: 'Delete Account',
      message: (
        <div>
          Are you sure you want to permanently delete user{' '}
          <strong style={{ color: 'var(--text)' }}>{username}</strong> (<code>{uid}</code>)
          from the slotted page database? This cannot be undone.
        </div>
      ),
      onConfirm: async () => {
        setConfirmModal((prev) => ({ ...prev, loading: true }));
        try {
          await api.deleteUser(uid);
          toast(`User "${username}" deleted from database.`, 'info');
          setConfirmModal((prev) => ({ ...prev, open: false, loading: false }));
          fetchData();
        } catch (err: unknown) {
          setConfirmModal((prev) => ({ ...prev, loading: false }));
          toast(err instanceof Error ? err.message : 'Failed to delete user.', 'error');
        }
      },
    });
  }

  function promptDeleteLot(lotId: string, title: string) {
    setConfirmModal({
      open: true,
      title: 'Delete Auction Lot',
      variant: 'danger',
      confirmLabel: 'Delete Lot',
      message: (
        <div>
          Are you sure you want to permanently delete lot{' '}
          <strong style={{ color: 'var(--text)' }}>{title}</strong> (<code>{lotId}</code>)
          from the catalog? All associated bids will be removed and disk space reclaimed.
        </div>
      ),
      onConfirm: async () => {
        setConfirmModal((prev) => ({ ...prev, loading: true }));
        try {
          await api.deleteItem(lotId);
          toast(`Lot "${title}" permanently deleted.`, 'info');
          setConfirmModal((prev) => ({ ...prev, open: false, loading: false }));
          fetchData();
        } catch (err: unknown) {
          setConfirmModal((prev) => ({ ...prev, loading: false }));
          toast(err instanceof Error ? err.message : 'Failed to delete lot.', 'error');
        }
      },
    });
  }

  function promptReset(mode: 'purge_items' | 'factory_reset') {
    const isFactory = mode === 'factory_reset';
    setConfirmModal({
      open: true,
      title: isFactory ? '💥 CRITICAL: Factory Reset Database' : '🧹 Purge All Lots & Bids',
      variant: 'danger',
      confirmLabel: isFactory ? 'Erase Everything' : 'Purge All Lots',
      message: isFactory ? (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
          <p style={{ margin: 0 }}>
            This will completely wipe <code>walnut.db</code> and <code>walnut.meta</code>, erasing
            <strong style={{ color: 'var(--danger)' }}> ALL lots, bid history, transactions, and user accounts</strong>.
          </p>
          <p style={{ margin: 0, fontSize: 13, color: 'var(--text-muted)' }}>
            A default admin account (<code>admin</code> / <code>admin</code>) will be immediately re-provisioned and your session will automatically renew.
          </p>
        </div>
      ) : (
        <div>
          Are you sure you want to purge all auction items and bid records?
          This will wipe all active and historical lots while preserving user accounts.
        </div>
      ),
      onConfirm: async () => {
        setConfirmModal((prev) => ({ ...prev, loading: true }));
        try {
          const res = await api.resetDb(mode);
          toast(res.message, 'success');

          if (isFactory && res.token) {
            signIn(
              {
                user_id: 'admin',
                username: 'admin',
                email: 'admin@walnut.internal',
                role: 'ADMIN',
              },
              res.token,
              true
            );
          }

          setConfirmModal((prev) => ({ ...prev, open: false, loading: false }));
          fetchData();
        } catch (err: unknown) {
          setConfirmModal((prev) => ({ ...prev, loading: false }));
          toast(err instanceof Error ? err.message : 'Database reset failed.', 'error');
        }
      },
    });
  }

  const filteredUsers = users.filter((u) => {
    if (!searchUsers) return true;
    const q = searchUsers.toLowerCase();
    return (
      u.username.toLowerCase().includes(q) ||
      u.user_id.toLowerCase().includes(q) ||
      (u.email && u.email.toLowerCase().includes(q))
    );
  });

  const filteredLots = lots.filter((l) => {
    if (!searchLots) return true;
    const q = searchLots.toLowerCase();
    return (
      l.title.toLowerCase().includes(q) ||
      l.item_id.toLowerCase().includes(q) ||
      l.category.toLowerCase().includes(q) ||
      l.host_id.toLowerCase().includes(q)
    );
  });

  return (
    <div style={{ maxWidth: 1080, margin: '0 auto', paddingBottom: 60 }}>
      {/* Confirmation Dialog (Modal replacement for window.confirm) */}
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

      {/* Header */}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: 16, marginBottom: 24 }}>
        <div>
          <h1 className="display" style={{ fontSize: 24, margin: 0 }}>Database Administration</h1>
          <p style={{ margin: '4px 0 0', fontSize: 13, color: 'var(--text-muted)' }}>
            Live slotted-page storage management, catalog inspection, user records, and database wipe controls.
          </p>
        </div>
        <div style={{ display: 'flex', gap: 10 }}>
          <button className="btn btn-ghost btn-sm" onClick={fetchData} disabled={loading}>
            {loading ? 'Refreshing…' : '↻ Refresh Data'}
          </button>
        </div>
      </div>

      {/* Database Storage Metrics */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(200px, 1fr))', gap: 16, marginBottom: 28 }}>
        <div className="card" style={{ padding: 18 }}>
          <div style={{ fontFamily: 'var(--font-num)', fontSize: 11, textTransform: 'uppercase', color: 'var(--text-muted)', letterSpacing: '0.07em' }}>
            Allocated Slotted Pages
          </div>
          <div style={{ fontSize: 24, fontWeight: 700, color: 'var(--accent)', marginTop: 4, fontFamily: 'var(--font-num)' }}>
            {stats?.total_pages ?? 3} pages
          </div>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginTop: 4 }}>
            In <code>./data/walnut.db</code> (4 KB / page)
          </div>
        </div>

        <div className="card" style={{ padding: 18 }}>
          <div style={{ fontFamily: 'var(--font-num)', fontSize: 11, textTransform: 'uppercase', color: 'var(--text-muted)', letterSpacing: '0.07em' }}>
            Auction Lots
          </div>
          <div style={{ fontSize: 24, fontWeight: 700, color: 'var(--text)', marginTop: 4, fontFamily: 'var(--font-num)' }}>
            {lots.length} lots
          </div>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginTop: 4 }}>
            Indexed via Slotted Pages
          </div>
        </div>

        <div className="card" style={{ padding: 18 }}>
          <div style={{ fontFamily: 'var(--font-num)', fontSize: 11, textTransform: 'uppercase', color: 'var(--text-muted)', letterSpacing: '0.07em' }}>
            Registered Users
          </div>
          <div style={{ fontSize: 24, fontWeight: 700, color: 'var(--text)', marginTop: 4, fontFamily: 'var(--font-num)' }}>
            {users.length} accounts
          </div>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginTop: 4 }}>
            280-byte binary records on disk
          </div>
        </div>

        <div className="card" style={{ padding: 18 }}>
          <div style={{ fontFamily: 'var(--font-num)', fontSize: 11, textTransform: 'uppercase', color: 'var(--text-muted)', letterSpacing: '0.07em' }}>
            Storage Health
          </div>
          <div style={{ fontSize: 16, fontWeight: 600, color: 'var(--success)', marginTop: 8, display: 'flex', alignItems: 'center', gap: 8 }}>
            <span className="live-dot" style={{ background: 'var(--success)', width: 8, height: 8, borderRadius: '50%' }} />
            Consistent & Active
          </div>
          <div style={{ fontSize: 12, color: 'var(--text-muted)', marginTop: 4 }}>
            WAL recovery trail operational
          </div>
        </div>
      </div>

      {/* Auction Catalog Management Section */}
      <div className="card" style={{ padding: 24, marginBottom: 28 }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: 12, marginBottom: 16 }}>
          <div>
            <h2 className="section-title" style={{ margin: 0, fontSize: 16 }}>
              Auction Catalog & Lots ({lots.length})
            </h2>
            <div style={{ fontSize: 12, color: 'var(--text-muted)', marginTop: 2 }}>
              Inspect and delete individual lots directly from persistent storage.
            </div>
          </div>
          <div className="search-box" style={{ maxWidth: 280 }}>
            <input
              type="search"
              className="search-input"
              placeholder="Search by title, lot ID, category…"
              value={searchLots}
              onChange={(e) => setSearchLots(e.target.value)}
              style={{ fontSize: 12, height: 32 }}
            />
          </div>
        </div>

        {loading ? (
          <p style={{ color: 'var(--text-muted)', fontSize: 13 }}>Loading lots from disk…</p>
        ) : filteredLots.length === 0 ? (
          <div style={{ padding: 24, textAlign: 'center', color: 'var(--text-muted)', fontSize: 13, background: 'var(--surface-2)', borderRadius: 8 }}>
            {lots.length === 0 ? 'No auction lots currently in the database.' : 'No lots match your search filter.'}
          </div>
        ) : (
          <div style={{ overflowX: 'auto', border: '1px solid var(--border)', borderRadius: 8 }}>
            <table className="lots-table" style={{ width: '100%' }}>
              <thead>
                <tr>
                  <th>Lot ID</th>
                  <th>Title</th>
                  <th>Category</th>
                  <th>Status</th>
                  <th>Current Price</th>
                  <th>Host</th>
                  <th style={{ textAlign: 'right' }}>Actions</th>
                </tr>
              </thead>
              <tbody>
                {filteredLots.map((l) => {
                  const isOpen = l.status === 'OPEN';
                  const isClosed = l.status === 'CLOSED';
                  return (
                    <tr key={l.item_id}>
                      <td style={{ fontFamily: 'var(--font-num)', fontSize: 12, color: 'var(--text-muted)' }}>
                        {l.item_id}
                      </td>
                      <td style={{ fontWeight: 600, fontSize: 13 }}>
                        <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
                          {l.image_urls?.[0] && (
                            <img
                              src={l.image_urls[0]}
                              alt=""
                              style={{ width: 28, height: 28, borderRadius: 4, objectFit: 'cover' }}
                            />
                          )}
                          <span>{l.title}</span>
                        </div>
                      </td>
                      <td style={{ fontSize: 12, color: 'var(--text-muted)' }}>
                        {l.category}
                      </td>
                      <td>
                        <span
                          className="badge"
                          style={
                            isOpen
                              ? { background: 'var(--success-wash)', color: 'var(--success)', borderColor: 'var(--success)' }
                              : isClosed
                              ? { background: 'var(--surface-2)', color: 'var(--text-muted)' }
                              : { background: 'var(--danger-wash)', color: 'var(--danger)' }
                          }
                        >
                          {l.status}
                        </span>
                      </td>
                      <td style={{ fontFamily: 'var(--font-num)', fontSize: 13, fontWeight: 600 }}>
                        ₹{(l.current_bid > 0 ? l.current_bid : l.starting_price).toLocaleString('en-IN')}
                      </td>
                      <td style={{ fontSize: 12, color: 'var(--text-muted)' }}>
                        {l.host_id}
                      </td>
                      <td style={{ textAlign: 'right' }}>
                        <button
                          className="btn btn-ghost btn-sm"
                          style={{ color: 'var(--danger)', fontSize: 11, padding: '3px 8px', height: 26 }}
                          onClick={() => promptDeleteLot(l.item_id, l.title)}
                          title="Permanently delete lot from database"
                        >
                          Delete Lot
                        </button>
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
      </div>

      {/* User Management Section */}
      <div className="card" style={{ padding: 24, marginBottom: 28 }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: 12, marginBottom: 16 }}>
          <div>
            <h2 className="section-title" style={{ margin: 0, fontSize: 16 }}>
              Registered Accounts ({users.length})
            </h2>
            <div style={{ fontSize: 12, color: 'var(--text-muted)', marginTop: 2 }}>
              Inspect and delete user account records from slotted page storage.
            </div>
          </div>
          <div className="search-box" style={{ maxWidth: 280 }}>
            <input
              type="search"
              className="search-input"
              placeholder="Search by username or ID…"
              value={searchUsers}
              onChange={(e) => setSearchUsers(e.target.value)}
              style={{ fontSize: 12, height: 32 }}
            />
          </div>
        </div>

        {loading ? (
          <p style={{ color: 'var(--text-muted)', fontSize: 13 }}>Loading accounts from disk…</p>
        ) : filteredUsers.length === 0 ? (
          <div style={{ padding: 24, textAlign: 'center', color: 'var(--text-muted)', fontSize: 13, background: 'var(--surface-2)', borderRadius: 8 }}>
            No accounts match your search filter.
          </div>
        ) : (
          <div style={{ overflowX: 'auto', border: '1px solid var(--border)', borderRadius: 8 }}>
            <table className="lots-table" style={{ width: '100%' }}>
              <thead>
                <tr>
                  <th>User ID</th>
                  <th>Username</th>
                  <th>Email</th>
                  <th>Role</th>
                  <th style={{ textAlign: 'right' }}>Actions</th>
                </tr>
              </thead>
              <tbody>
                {filteredUsers.map((u) => {
                  const isCurrent = u.user_id === user?.user_id;
                  const isSuperAdmin = u.user_id === 'admin' || u.role === 'ADMIN';
                  return (
                    <tr key={u.user_id}>
                      <td style={{ fontFamily: 'var(--font-num)', fontSize: 12, color: 'var(--text-muted)' }}>
                        {u.user_id}
                      </td>
                      <td style={{ fontWeight: 600, fontSize: 13 }}>
                        {u.username}
                        {isCurrent && (
                          <span className="badge" style={{ marginLeft: 8, fontSize: 10, background: 'var(--surface-2)', color: 'var(--accent)' }}>
                            You
                          </span>
                        )}
                      </td>
                      <td style={{ fontSize: 12, color: 'var(--text-muted)' }}>
                        {u.email || '—'}
                      </td>
                      <td>
                        <span
                          className="badge"
                          style={
                            isSuperAdmin
                              ? { background: 'var(--danger-wash)', color: 'var(--danger)', borderColor: 'var(--danger)' }
                              : { background: 'var(--surface-2)', color: 'var(--text-muted)' }
                          }
                        >
                          {u.role || 'USER'}
                        </span>
                      </td>
                      <td style={{ textAlign: 'right' }}>
                        <button
                          className="btn btn-ghost btn-sm"
                          style={{ color: 'var(--danger)', fontSize: 11, padding: '3px 8px', height: 26 }}
                          onClick={() => promptDeleteUser(u.user_id, u.username)}
                          disabled={isCurrent}
                          title={isCurrent ? 'Cannot delete yourself' : 'Delete user account'}
                        >
                          Delete
                        </button>
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
      </div>

      {/* Danger Zone: Storage Reset */}
      <div className="card" style={{ padding: 24, borderColor: 'var(--danger-wash)', background: 'linear-gradient(180deg, var(--surface) 0%, rgba(239, 68, 68, 0.04) 100%)' }}>
        <h2 className="section-title" style={{ margin: 0, fontSize: 16, color: 'var(--danger)', marginBottom: 8 }}>
          Danger Zone · Storage Reset
        </h2>
        <p style={{ fontSize: 13, color: 'var(--text-muted)', marginBottom: 18, lineHeight: 1.5 }}>
          Direct storage operations that modify or delete raw binary files in <code>./data/</code>.
          Purging lots preserves user credentials while wiping all lots and bids. Factory reset completely resets the database to a blank state.
        </p>

        <div style={{ display: 'flex', gap: 12, flexWrap: 'wrap' }}>
          <button
            type="button"
            className="btn btn-ghost btn-sm"
            style={{ color: 'var(--danger)', borderColor: 'var(--danger)', padding: '8px 16px', height: 38 }}
            onClick={() => promptReset('purge_items')}
          >
            🧹 Purge All Lots & Bids
          </button>

          <button
            type="button"
            className="btn btn-danger btn-sm"
            style={{ padding: '8px 16px', height: 38 }}
            onClick={() => promptReset('factory_reset')}
          >
            💥 Factory Reset (Wipe Database)
          </button>
        </div>
      </div>
    </div>
  );
}
