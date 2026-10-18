import { useState, useRef, useEffect } from 'react';
import { NavLink, useNavigate, Outlet } from 'react-router-dom';
import { useSession } from '../context/SessionContext';
import { useStream } from '../context/StreamContext';
import { useToast } from '../context/ToastContext';
import { auth } from '../lib/api';
import { ConnectionPill, PasswordInput } from './Primitives';
import { EngineDrawer } from './EngineDrawer';
import { sha256 } from '../lib/crypto';

// SVG icons (clean inline icons)
const IconFloor   = () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} strokeLinecap="round" strokeLinejoin="round"><rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/></svg>;
const IconHost    = () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} strokeLinecap="round" strokeLinejoin="round"><path d="M12 2l7 4v6c0 5-3.5 9.74-7 11-3.5-1.26-7-6-7-11V6l7-4z"/></svg>;
const IconTrophy  = () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} strokeLinecap="round" strokeLinejoin="round"><path d="M6 9H4a2 2 0 01-2-2V5h4M18 9h2a2 2 0 002-2V5h-4"/><path d="M6 9c0 6 6 9 6 9s6-3 6-9"/><path d="M12 18v3M8 21h8"/></svg>;
const IconEngine  = () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} strokeLinecap="round" strokeLinejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.07 4.93l-1.41 1.41M4.93 4.93l1.41 1.41M19.07 19.07l-1.41-1.41M4.93 19.07l1.41-1.41M12 2v2M12 20v2M2 12h2M20 12h2"/></svg>;
const IconAdmin   = () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} strokeLinecap="round" strokeLinejoin="round"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/><circle cx="12" cy="11" r="2.5"/></svg>;
const IconProfile = () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} strokeLinecap="round" strokeLinejoin="round"><path d="M20 21v-2a4 4 0 00-4-4H8a4 4 0 00-4 4v2"/><circle cx="12" cy="7" r="4"/></svg>;
const IconChevronLeft  = () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={2} strokeLinecap="round" strokeLinejoin="round"><polyline points="15 18 9 12 15 6"/></svg>;
const IconChevronRight = () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={2} strokeLinecap="round" strokeLinejoin="round"><polyline points="9 18 15 12 9 6"/></svg>;
const IconMenu = () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={2} strokeLinecap="round" strokeLinejoin="round"><line x1="3" y1="12" x2="21" y2="12"/><line x1="3" y1="6" x2="21" y2="6"/><line x1="3" y1="18" x2="21" y2="18"/></svg>;
const IconSun  = () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} strokeLinecap="round" strokeLinejoin="round"><circle cx="12" cy="12" r="5"/><line x1="12" y1="1" x2="12" y2="3"/><line x1="12" y1="21" x2="12" y2="23"/><line x1="4.22" y1="4.22" x2="5.64" y2="5.64"/><line x1="18.36" y1="18.36" x2="19.78" y2="19.78"/><line x1="1" y1="12" x2="3" y2="12"/><line x1="21" y1="12" x2="23" y2="12"/><line x1="4.22" y1="19.78" x2="5.64" y2="18.36"/><line x1="18.36" y1="5.64" x2="19.78" y2="4.22"/></svg>;
const IconMoon = () => <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} strokeLinecap="round" strokeLinejoin="round"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/></svg>;

import { WalnutFullLogo } from './Logo';


export function AppShell() {
  const { user, signOut } = useSession();
  const { status } = useStream();
  const navigate = useNavigate();

  const isAdmin = user?.role === 'ADMIN' || user?.username === 'admin';

  // Retractable side panel state
  const [sidebarCollapsed, setSidebarCollapsed] = useState(() =>
    localStorage.getItem('wn_sidebar_collapsed') === '1'
  );
  const [mobileSidebarOpen, setMobileSidebarOpen] = useState(false);

  const [drawerOpen, setDrawerOpen] = useState(() => localStorage.getItem('wn_drawer') === '1');
  const [activeLsn, setActiveLsn] = useState<string | undefined>();
  const [userMenuOpen, setUserMenuOpen] = useState(false);
  const [passwordModalOpen, setPasswordModalOpen] = useState(false);
  const userMenuRef = useRef<HTMLDivElement>(null);

  // Theme state: dark | light
  const [theme, setTheme] = useState<'dark' | 'light'>(() => {
    const saved = localStorage.getItem('wn_theme');
    if (saved === 'light' || saved === 'dark') return saved;
    return document.documentElement.getAttribute('data-theme') === 'light' ? 'light' : 'dark';
  });

  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme);
    localStorage.setItem('wn_theme', theme);
  }, [theme]);

  const toggleTheme = () => {
    setTheme((prev) => (prev === 'dark' ? 'light' : 'dark'));
  };

  // Redirect unauthenticated visitors to Gateway
  useEffect(() => {
    if (!user) {
      navigate('/', { replace: true });
    }
  }, [user, navigate]);

  // Persist sidebar state
  useEffect(() => {
    localStorage.setItem('wn_sidebar_collapsed', sidebarCollapsed ? '1' : '0');
  }, [sidebarCollapsed]);

  // Persist drawer state
  useEffect(() => {
    localStorage.setItem('wn_drawer', drawerOpen ? '1' : '0');
  }, [drawerOpen]);

  // Close user menu on outside click
  useEffect(() => {
    function handler(e: MouseEvent) {
      if (userMenuRef.current && !userMenuRef.current.contains(e.target as Node)) {
        setUserMenuOpen(false);
      }
    }
    document.addEventListener('mousedown', handler);
    return () => document.removeEventListener('mousedown', handler);
  }, []);

  function openEngineAt(lsn: string) {
    setActiveLsn(lsn);
    setDrawerOpen(true);
  }

  function handleSignOut() {
    signOut();
    navigate('/');
  }

  const initials = user?.username.slice(0, 2).toUpperCase() ?? '?';

  return (
    <div className={`app-shell ${sidebarCollapsed ? 'sidebar-collapsed' : 'sidebar-expanded'}`}>
      {/* ── 1. Top Navbar (Sits across the top of the entire UI) ── */}
      <header className="navbar" role="navigation" aria-label="Top navigation">
        <div className="navbar-left">
          {/* Mobile hamburger menu button */}
          <button
            className="icon-btn mobile-hamburger"
            onClick={() => setMobileSidebarOpen((o) => !o)}
            aria-label="Toggle navigation"
          >
            <span style={{ width: 20, height: 20, display: 'inline-flex', alignItems: 'center', justifyContent: 'center' }}>
              <IconMenu />
            </span>
          </button>

          {/* Complete WALnut logo on top */}
          <NavLink to="/auction" className="navbar-logo-link" aria-label="WALnut home">
            <WalnutFullLogo height={32} />
          </NavLink>
        </div>

        <div className="navbar-right">
          {/* Animated pulsating live beacon */}
          <ConnectionPill status={status} />

          {/* User Chip & Dropdown */}
          {user ? (
            <div style={{ position: 'relative' }} ref={userMenuRef}>
              <button
                className="user-chip"
                onClick={() => setUserMenuOpen((o) => !o)}
                aria-expanded={userMenuOpen}
                aria-haspopup="true"
              >
                <div className="user-avatar">{initials}</div>
                <span className="user-name">{user.username}</span>
              </button>

              {userMenuOpen && (
                <div className="user-dropdown">
                  <div className="user-dropdown-header">
                    <div className="user-dropdown-name">{user.username}</div>
                    <div className="user-dropdown-email">{user.email || 'No email provided'}</div>
                  </div>
                  <button className="dropdown-item" onClick={() => { navigate('/profile'); setUserMenuOpen(false); }}>
                    <span style={{ width: 16, height: 16, display: 'inline-flex' }}><IconProfile /></span>
                    My Profile & Settings
                  </button>
                  <button className="dropdown-item" onClick={() => { navigate('/won'); setUserMenuOpen(false); }}>
                    <span style={{ width: 16, height: 16, display: 'inline-flex' }}><IconTrophy /></span>
                    Won items
                  </button>
                  <button className="dropdown-item" onClick={() => { navigate('/host'); setUserMenuOpen(false); }}>
                    <span style={{ width: 16, height: 16, display: 'inline-flex' }}><IconHost /></span>
                    Host console
                  </button>
                  <button className="dropdown-item" onClick={() => { navigate('/engine'); setUserMenuOpen(false); }}>
                    <span style={{ width: 16, height: 16, display: 'inline-flex' }}><IconEngine /></span>
                    Engine explorer
                  </button>
                  {(user.role === 'ADMIN' || user.username === 'admin') && (
                    <button className="dropdown-item" onClick={() => { navigate('/admin'); setUserMenuOpen(false); }}>
                      🛡 Database & Admin Controls
                    </button>
                  )}
                  <button className="dropdown-item" onClick={() => { toggleTheme(); setUserMenuOpen(false); }}>
                    <span style={{ width: 16, height: 16, display: 'inline-flex' }}>
                      {theme === 'dark' ? <IconSun /> : <IconMoon />}
                    </span>
                    {theme === 'dark' ? 'Switch to Light mode' : 'Switch to Dark mode'}
                  </button>
                  <button className="dropdown-item" onClick={() => { setPasswordModalOpen(true); setUserMenuOpen(false); }}>
                    🔑 Change password
                  </button>
                  <button className="dropdown-item danger" onClick={handleSignOut}>
                    Sign out
                  </button>
                </div>
              )}
            </div>
          ) : (
            <button
              className="btn btn-ghost btn-sm"
              onClick={() => navigate('/')}
              style={{ fontSize: 13, padding: '6px 14px' }}
            >
              Sign in
            </button>
          )}
        </div>
      </header>

      {/* ── 2. App Body: Sidebar (strictly below top bar) + Page Content ── */}
      <div className="app-body">
        {/* Mobile Backdrop */}
        {mobileSidebarOpen && (
          <div
            className="sidebar-backdrop"
            onClick={() => setMobileSidebarOpen(false)}
            aria-hidden="true"
          />
        )}

        {/* Retractable Side Panel - starts from below navbar, NO logo in sidebar */}
        <aside
          className={`app-sidebar ${sidebarCollapsed ? 'collapsed' : 'expanded'} ${mobileSidebarOpen ? 'mobile-open' : ''}`}
          aria-label="Main navigation"
        >
          {/* Mobile drawer header (only shown on mobile screens) */}
          <div className="mobile-drawer-header">
            <span className="mobile-drawer-title">Navigation</span>
            <button
              className="sidebar-close-btn"
              onClick={() => setMobileSidebarOpen(false)}
              aria-label="Close navigation"
            >
              ✕
            </button>
          </div>

          {/* Desktop collapse/expand toggle sitting cleanly inside the sidebar */}
          <div className="sidebar-toggle-row desktop-only">
            <button
              className="sidebar-toggle-btn"
              onClick={() => setSidebarCollapsed((c) => !c)}
              aria-label={sidebarCollapsed ? 'Expand sidebar' : 'Collapse sidebar'}
              title={sidebarCollapsed ? 'Expand sidebar' : 'Collapse sidebar'}
            >
              <span className="sidebar-toggle-icon">
                {sidebarCollapsed ? <IconChevronRight /> : <IconChevronLeft />}
              </span>
              <span className="sidebar-toggle-label">Collapse</span>
            </button>
          </div>

          <nav className="sidebar-nav">
            <div className="sidebar-group-label">AUCTION</div>
            <NavLink
              to="/auction"
              className={({ isActive }) => `sidebar-link${isActive ? ' active' : ''}`}
              title="Floor"
              onClick={() => setMobileSidebarOpen(false)}
            >
              <span className="sidebar-icon"><IconFloor /></span>
              <span className="sidebar-label">Floor</span>
            </NavLink>

            <NavLink
              to="/host"
              className={({ isActive }) => `sidebar-link${isActive ? ' active' : ''}`}
              title="Host & Past Listings"
              onClick={() => setMobileSidebarOpen(false)}
            >
              <span className="sidebar-icon"><IconHost /></span>
              <span className="sidebar-label">Host</span>
            </NavLink>

            <NavLink
              to="/won"
              className={({ isActive }) => `sidebar-link${isActive ? ' active' : ''}`}
              title="Won Items"
              onClick={() => setMobileSidebarOpen(false)}
            >
              <span className="sidebar-icon"><IconTrophy /></span>
              <span className="sidebar-label">Won items</span>
            </NavLink>

            <div className="sidebar-group-label">SYSTEM</div>
            <NavLink
              to="/engine"
              className={({ isActive }) => `sidebar-link${isActive ? ' active' : ''}`}
              title="Engine"
              onClick={() => setMobileSidebarOpen(false)}
            >
              <span className="sidebar-icon"><IconEngine /></span>
              <span className="sidebar-label">Engine</span>
            </NavLink>

            {isAdmin && (
              <NavLink
                to="/admin"
                className={({ isActive }) => `sidebar-link${isActive ? ' active' : ''}`}
                title="Database Administration"
                onClick={() => setMobileSidebarOpen(false)}
              >
                <span className="sidebar-icon"><IconAdmin /></span>
                <span className="sidebar-label">Admin</span>
              </NavLink>
            )}
          </nav>

          <div className="sidebar-footer">
            <button
              className="sidebar-theme-toggle"
              onClick={toggleTheme}
              title={theme === 'dark' ? 'Switch to light mode' : 'Switch to dark mode'}
              aria-label="Toggle theme"
            >
              <span className="sidebar-icon">
                {theme === 'dark' ? <IconSun /> : <IconMoon />}
              </span>
              <span className="sidebar-label">
                {theme === 'dark' ? 'Light mode' : 'Dark mode'}
              </span>
              <span className="sidebar-theme-badge">
                {theme === 'dark' ? 'Dark' : 'Light'}
              </span>
            </button>

            {user && (
              <div
                className="sidebar-user-card"
                onClick={() => { navigate('/profile'); setMobileSidebarOpen(false); }}
                title="View & Edit Profile"
              >
                <div className="user-avatar">{initials}</div>
                <div className="sidebar-user-details">
                  <div className="sidebar-user-name">{user.username}</div>
                  <div className="sidebar-user-role">{user.role || 'USER'}</div>
                </div>
              </div>
            )}
          </div>
        </aside>

        {/* ── Main Page Content (Injected by router) ── */}
        <main className="page-content" id="main-content">
          <Outlet context={{ openEngineAt }} />
        </main>
      </div>

      {/* ── Mobile bottom bar ── */}
      <nav className="mobile-bar" aria-label="Mobile navigation">
        {[
          { to: '/auction', icon: <IconFloor />, label: 'Floor' },
          { to: '/host',    icon: <IconHost />,   label: 'Host' },
          { to: '/won',     icon: <IconTrophy />, label: 'Won' },
          { to: '/engine',  icon: <IconEngine />, label: 'Engine' },
          { to: '/profile', icon: <IconProfile />, label: 'Profile' },
        ].map((item) => (
          <NavLink
            key={item.to}
            to={item.to}
            className={({ isActive }) => `mobile-tab${isActive ? ' active' : ''}`}
          >
            {item.icon}
            {item.label}
          </NavLink>
        ))}
      </nav>

      {/* ── Engine drawer ── */}
      {drawerOpen && (
        <EngineDrawer
          onClose={() => setDrawerOpen(false)}
          activeLsn={activeLsn}
          onNavigateEngine={() => { navigate('/engine'); setDrawerOpen(false); }}
        />
      )}

      {/* ── Change Password Modal ── */}
      <ChangePasswordModal
        isOpen={passwordModalOpen}
        onClose={() => setPasswordModalOpen(false)}
      />
    </div>
  );
}

function ChangePasswordModal({ isOpen, onClose }: { isOpen: boolean; onClose: () => void }) {
  const toast = useToast();
  const [oldPw, setOldPw] = useState('');
  const [newPw, setNewPw] = useState('');
  const [confirmPw, setConfirmPw] = useState('');
  const [showNewPw, setShowNewPw] = useState(false);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    if (!isOpen) {
      setOldPw('');
      setNewPw('');
      setConfirmPw('');
      setShowNewPw(false);
      setError(null);
    }
  }, [isOpen]);

  if (!isOpen) return null;

  async function submit(e: React.FormEvent) {
    e.preventDefault();
    setError(null);
    if (!newPw || newPw.length < 8) {
      setError('New password must be at least 8 characters.');
      return;
    }
    if (newPw !== confirmPw) {
      setError('New passwords do not match.');
      return;
    }

    setPending(true);
    try {
      const oldHash = oldPw ? await sha256(oldPw) : undefined;
      const newHash = await sha256(newPw);
      await auth.changePassword({ old_password: oldHash, new_password: newHash });
      toast('Password changed successfully!', 'success');
      onClose();
    } catch (err: unknown) {
      setError(err instanceof Error ? err.message : 'Failed to update password.');
    } finally {
      setPending(false);
    }
  }

  return (
    <div
      className="modal-backdrop"
      role="dialog"
      aria-modal="true"
      onClick={(e) => { if (e.target === e.currentTarget) onClose(); }}
    >
      <div className="modal" style={{ maxWidth: 440, padding: 24, display: 'block' }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 16 }}>
          <h2 className="display" style={{ fontSize: 20 }}>Change Password</h2>
          <button className="btn btn-ghost" onClick={onClose} style={{ padding: '4px 8px' }}>✕</button>
        </div>

        <form onSubmit={submit}>
          <div className="form-group" style={{ marginBottom: 14 }}>
            <label className="form-label" htmlFor="cp-old">Current password</label>
            <PasswordInput
              id="cp-old"
              value={oldPw}
              onChange={(e) => setOldPw(e.target.value)}
              placeholder="••••••••"
              autoComplete="current-password"
            />
          </div>

          <div className="form-group" style={{ marginBottom: 14 }}>
            <label className="form-label" htmlFor="cp-new">New password</label>
            <input
              id="cp-new"
              type={showNewPw ? 'text' : 'password'}
              className="form-input"
              value={newPw}
              onChange={(e) => setNewPw(e.target.value)}
              placeholder="≥ 8 chars"
              autoComplete="new-password"
            />
          </div>

          <div className="form-group" style={{ marginBottom: 16 }}>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
              <label className="form-label" htmlFor="cp-confirm" style={{ marginBottom: 0 }}>Confirm new password</label>
              <button
                type="button"
                className="password-toggle-link"
                onClick={() => setShowNewPw((s) => !s)}
                aria-label={showNewPw ? 'Hide passwords' : 'Show passwords'}
                tabIndex={-1}
              >
                {showNewPw ? (
                  <>
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} width="14" height="14">
                      <path d="M17.94 17.94A10.07 10.07 0 0112 20c-7 0-11-8-11-8a18.45 18.45 0 015.06-5.94M9.9 4.24A9.12 9.12 0 0112 4c7 0 11 8 11 8a18.5 18.5 0 01-2.16 3.19m-6.72-1.07a3 3 0 11-4.24-4.24" strokeLinecap="round" strokeLinejoin="round"/>
                      <line x1="1" y1="1" x2="23" y2="23" strokeLinecap="round" strokeLinejoin="round"/>
                    </svg>
                    <span>Hide</span>
                  </>
                ) : (
                  <>
                    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} width="14" height="14">
                      <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z" strokeLinecap="round" strokeLinejoin="round"/>
                      <circle cx="12" cy="12" r="3"/>
                    </svg>
                    <span>Show</span>
                  </>
                )}
              </button>
            </div>
            <input
              id="cp-confirm"
              type={showNewPw ? 'text' : 'password'}
              className="form-input"
              value={confirmPw}
              onChange={(e) => setConfirmPw(e.target.value)}
              placeholder="same again"
              autoComplete="new-password"
            />
          </div>

          {error && (
            <div className="form-error" role="alert" style={{ marginBottom: 16 }}>
              <span aria-hidden>⚠</span> {error}
            </div>
          )}

          <div style={{ display: 'flex', gap: 10, justifyContent: 'flex-end', marginTop: 20 }}>
            <button type="button" className="btn btn-ghost" onClick={onClose} disabled={pending}>
              Cancel
            </button>
            <button type="submit" className="btn btn-primary" disabled={pending}>
              {pending ? 'Updating…' : 'Update password'}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}
