// Gateway – Sign In / Sign Up with SHA-256 hashing before sending to server.
// This is the entry point for unauthenticated users.

import { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { auth } from '../lib/api';
import { useSession } from '../context/SessionContext';
import { useStream } from '../context/StreamContext';
import { useToast } from '../context/ToastContext';
import { ConnectionPill, PasswordInput } from '../components/Primitives';

import { WalnutFullLogo } from '../components/Logo';


import { sha256 } from '../lib/crypto';

export function Gateway() {
  const navigate = useNavigate();
  const { user, signIn, signOut } = useSession();
  const { status } = useStream();
  const toast = useToast();

  // Do not auto-redirect away: visiting / always displays the Sign In / Sign Up Gateway

  const [tab, setTab] = useState<'signin' | 'signup' | 'reset'>('signin');
  const [remember, setRemember] = useState(() => localStorage.getItem('wn_remember') !== '0');
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [success, setSuccess] = useState<string | null>(null);

  // Sign-in fields
  const [siUsername, setSiUsername] = useState(() => localStorage.getItem('wn_saved_username') ?? '');
  const [siPassword, setSiPassword] = useState('');

  // Sign-up fields
  const [suUsername, setSuUsername] = useState('');
  const [suEmail, setSuEmail]       = useState('');
  const [suPhone, setSuPhone]       = useState('');
  const [suPassword, setSuPassword] = useState('');
  const [suConfirm, setSuConfirm]   = useState('');
  const [showSuPassword, setShowSuPassword] = useState(false);

  // Reset fields
  const [rstUsername, setRstUsername] = useState('');
  const [rstEmail, setRstEmail]       = useState('');
  const [rstPassword, setRstPassword] = useState('');
  const [rstConfirm, setRstConfirm]   = useState('');
  const [showRstPassword, setShowRstPassword] = useState(false);

  function validate(): string | null {
    if (tab === 'signin') {
      if (!siUsername.trim()) return 'Username is required.';
      if (!siPassword) return 'Password is required.';
    } else if (tab === 'signup') {
      if (!suUsername.trim()) return 'Username is required.';
      if (!/^[a-zA-Z0-9_]{2,32}$/.test(suUsername)) return 'Username: 2–32 chars, letters/digits/underscore.';
      if (!suEmail.trim() || !suEmail.includes('@')) return 'A valid email is required.';
      if (!suPassword || suPassword.length < 8) return 'Password must be at least 8 characters.';
      if (suPassword !== suConfirm) return 'Passwords do not match.';
    } else if (tab === 'reset') {
      if (!rstUsername.trim()) return 'Username handle is required.';
      if (!rstPassword || rstPassword.length < 8) return 'New password must be at least 8 characters.';
      if (rstPassword !== rstConfirm) return 'Passwords do not match.';
    }
    return null;
  }

  async function submit(e: React.FormEvent) {
    e.preventDefault();
    setError(null);
    setSuccess(null);
    const err = validate();
    if (err) { setError(err); return; }

    setPending(true);
    try {
      if (tab === 'signin') {
        const hash = await sha256(siPassword);
        const res = await auth.signIn({ username: siUsername.trim(), password: hash });
        if (remember) {
          localStorage.setItem('wn_saved_username', res.username);
          localStorage.setItem('wn_remember', '1');
        } else {
          localStorage.removeItem('wn_saved_username');
          localStorage.setItem('wn_remember', '0');
        }
        signIn({ user_id: res.user_id, username: res.username, email: res.email, role: res.role }, res.token, remember);
        navigate('/auction', { replace: true });
      } else if (tab === 'signup') {
        const hash = await sha256(suPassword);
        const newUsername = suUsername.trim();
        signOut();
        await auth.signUp({
          username: newUsername,
          email: suEmail.trim().toLowerCase(),
          password: hash,
          phone: suPhone.trim() || undefined,
        });
        toast('Account created successfully! Please sign in with your credentials.', 'success');
        setSuccess(`Account "${newUsername}" created successfully! Please enter your password to sign in.`);
        setSiUsername(newUsername);
        setSiPassword('');
        setSuPassword('');
        setSuConfirm('');
        setTab('signin');
      } else {
        const hash = await sha256(rstPassword);
        const res = await auth.resetPassword({
          username: rstUsername.trim(),
          email: rstEmail.trim().toLowerCase() || undefined,
          new_password: hash,
        });
        if (remember) {
          localStorage.setItem('wn_saved_username', res.username);
          localStorage.setItem('wn_remember', '1');
        } else {
          localStorage.removeItem('wn_saved_username');
          localStorage.setItem('wn_remember', '0');
        }
        signIn({ user_id: res.user_id, username: res.username, email: res.email, role: res.role }, res.token, remember);
        navigate('/auction', { replace: true });
      }
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Something went wrong. Try again.';
      setError(msg);
    } finally {
      setPending(false);
    }
  }

  return (
    <div className="gateway-bg">
      <div className="gateway-card">
        {/* Logo */}
        <div className="gateway-logo" style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', marginBottom: 20 }}>
          <WalnutFullLogo height={44} />
          <div className="gateway-sub" style={{ marginTop: 6, fontSize: 13, color: 'var(--text-muted)' }}>
            Live auction house
          </div>
        </div>

        {/* Connection pill */}
        <div style={{ marginBottom: 20 }}>
          <ConnectionPill status={status} />
        </div>

        {user && (
          <div style={{
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'space-between',
            background: 'var(--surface-2)',
            border: '1px solid var(--accent)',
            borderRadius: 'var(--r-md)',
            padding: '10px 14px',
            marginBottom: 20,
          }}>
            <span style={{ fontSize: 13, color: 'var(--text)' }}>
              Signed in as <strong style={{ color: 'var(--accent)' }}>{user.username}</strong>
            </span>
            <button
              type="button"
              className="btn btn-primary btn-sm"
              onClick={() => navigate('/auction')}
              style={{ fontSize: 12, padding: '4px 12px', height: 30 }}
            >
              Enter Floor →
            </button>
          </div>
        )}

        {/* Tabs */}
        <div className="gateway-tabs" role="tablist">
          <button
            role="tab"
            aria-selected={tab === 'signin'}
            className={`gateway-tab${tab === 'signin' ? ' active' : ''}`}
            onClick={() => { setTab('signin'); setError(null); setSuccess(null); }}
          >
            Sign in
          </button>
          <button
            role="tab"
            aria-selected={tab === 'signup'}
            className={`gateway-tab${tab === 'signup' ? ' active' : ''}`}
            onClick={() => { setTab('signup'); setError(null); setSuccess(null); }}
          >
            Create account
          </button>
          {tab === 'reset' && (
            <button
              role="tab"
              aria-selected={true}
              className="gateway-tab active"
            >
              Reset password
            </button>
          )}
        </div>

        <form onSubmit={submit} noValidate>
          {tab === 'signin' ? (
            <>
              <div className="form-group">
                <label className="form-label" htmlFor="si-username">Username</label>
                <input
                  id="si-username"
                  type="text"
                  className={`form-input${error && !siUsername.trim() ? ' error' : ''}`}
                  value={siUsername}
                  onChange={(e) => setSiUsername(e.target.value)}
                  placeholder="your_handle"
                  autoComplete="username"
                  autoFocus
                />
              </div>
              <div className="form-group">
                <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
                  <label className="form-label" htmlFor="si-password">Password</label>
                  <button
                    type="button"
                    className="btn btn-ghost btn-sm"
                    style={{ padding: '0 4px', height: 'auto', fontSize: 12, color: 'var(--accent)' }}
                    onClick={() => { setTab('reset'); setError(null); }}
                  >
                    Forgot password?
                  </button>
                </div>
                <PasswordInput
                  id="si-password"
                  value={siPassword}
                  onChange={(e) => setSiPassword(e.target.value)}
                  placeholder="••••••••"
                  autoComplete="current-password"
                />
              </div>
            </>
          ) : tab === 'signup' ? (
            <>
              <div className="form-row">
                <div className="form-group">
                  <label className="form-label" htmlFor="su-username">Username</label>
                  <input
                    id="su-username"
                    type="text"
                    className="form-input"
                    value={suUsername}
                    onChange={(e) => setSuUsername(e.target.value)}
                    placeholder="your_handle"
                    autoComplete="username"
                    autoFocus
                  />
                </div>
                <div className="form-group">
                  <label className="form-label" htmlFor="su-phone">Phone (optional)</label>
                  <input
                    id="su-phone"
                    type="tel"
                    className="form-input"
                    value={suPhone}
                    onChange={(e) => setSuPhone(e.target.value)}
                    placeholder="+91 9876543210"
                    autoComplete="tel"
                  />
                </div>
              </div>
              <div className="form-group">
                <label className="form-label" htmlFor="su-email">Email</label>
                <input
                  id="su-email"
                  type="email"
                  className="form-input"
                  value={suEmail}
                  onChange={(e) => setSuEmail(e.target.value)}
                  placeholder="you@example.com"
                  autoComplete="email"
                />
              </div>
              <div className="form-row">
                <div className="form-group">
                  <label className="form-label" htmlFor="su-password">Password</label>
                  <input
                    id="su-password"
                    type={showSuPassword ? 'text' : 'password'}
                    className="form-input"
                    value={suPassword}
                    onChange={(e) => setSuPassword(e.target.value)}
                    placeholder="≥ 8 chars"
                    autoComplete="new-password"
                  />
                </div>
                <div className="form-group">
                  <label className="form-label" htmlFor="su-confirm">Confirm password</label>
                  <input
                    id="su-confirm"
                    type={showSuPassword ? 'text' : 'password'}
                    className="form-input"
                    value={suConfirm}
                    onChange={(e) => setSuConfirm(e.target.value)}
                    placeholder="same again"
                    autoComplete="new-password"
                  />
                </div>
              </div>

              <div style={{ marginTop: 6, marginBottom: 14 }}>
                <button
                  type="button"
                  className="password-toggle-link"
                  onClick={() => setShowSuPassword((s) => !s)}
                  aria-label={showSuPassword ? 'Hide password' : 'Show password'}
                  tabIndex={-1}
                >
                  {showSuPassword ? (
                    <>
                      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} width="15" height="15">
                        <path d="M17.94 17.94A10.07 10.07 0 0112 20c-7 0-11-8-11-8a18.45 18.45 0 015.06-5.94M9.9 4.24A9.12 9.12 0 0112 4c7 0 11 8 11 8a18.5 18.5 0 01-2.16 3.19m-6.72-1.07a3 3 0 11-4.24-4.24" strokeLinecap="round" strokeLinejoin="round"/>
                        <line x1="1" y1="1" x2="23" y2="23" strokeLinecap="round" strokeLinejoin="round"/>
                      </svg>
                      <span>Hide password</span>
                    </>
                  ) : (
                    <>
                      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} width="15" height="15">
                        <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z" strokeLinecap="round" strokeLinejoin="round"/>
                        <circle cx="12" cy="12" r="3"/>
                      </svg>
                      <span>Show password</span>
                    </>
                  )}
                </button>
              </div>
            </>
          ) : (
            <>
              <div className="form-group">
                <label className="form-label" htmlFor="rst-username">Username handle</label>
                <input
                  id="rst-username"
                  type="text"
                  className={`form-input${error && !rstUsername.trim() ? ' error' : ''}`}
                  value={rstUsername}
                  onChange={(e) => setRstUsername(e.target.value)}
                  placeholder="your_handle (e.g. vinayak)"
                  autoComplete="username"
                  autoFocus
                />
              </div>
              <div className="form-group">
                <label className="form-label" htmlFor="rst-email">Registered Email (optional if none set)</label>
                <input
                  id="rst-email"
                  type="email"
                  className="form-input"
                  value={rstEmail}
                  onChange={(e) => setRstEmail(e.target.value)}
                  placeholder="you@example.com"
                  autoComplete="email"
                />
              </div>
              <div className="form-row">
                <div className="form-group">
                  <label className="form-label" htmlFor="rst-password">New password</label>
                  <input
                    id="rst-password"
                    type={showRstPassword ? 'text' : 'password'}
                    className="form-input"
                    value={rstPassword}
                    onChange={(e) => setRstPassword(e.target.value)}
                    placeholder="≥ 8 chars"
                    autoComplete="new-password"
                  />
                </div>
                <div className="form-group">
                  <label className="form-label" htmlFor="rst-confirm">Confirm new password</label>
                  <input
                    id="rst-confirm"
                    type={showRstPassword ? 'text' : 'password'}
                    className="form-input"
                    value={rstConfirm}
                    onChange={(e) => setRstConfirm(e.target.value)}
                    placeholder="same again"
                    autoComplete="new-password"
                  />
                </div>
              </div>

              <div style={{ marginTop: 6, marginBottom: 14 }}>
                <button
                  type="button"
                  className="password-toggle-link"
                  onClick={() => setShowRstPassword((s) => !s)}
                  aria-label={showRstPassword ? 'Hide password' : 'Show password'}
                  tabIndex={-1}
                >
                  {showRstPassword ? (
                    <>
                      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} width="15" height="15">
                        <path d="M17.94 17.94A10.07 10.07 0 0112 20c-7 0-11-8-11-8a18.45 18.45 0 015.06-5.94M9.9 4.24A9.12 9.12 0 0112 4c7 0 11 8 11 8a18.5 18.5 0 01-2.16 3.19m-6.72-1.07a3 3 0 11-4.24-4.24" strokeLinecap="round" strokeLinejoin="round"/>
                        <line x1="1" y1="1" x2="23" y2="23" strokeLinecap="round" strokeLinejoin="round"/>
                      </svg>
                      <span>Hide password</span>
                    </>
                  ) : (
                    <>
                      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} width="15" height="15">
                        <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z" strokeLinecap="round" strokeLinejoin="round"/>
                        <circle cx="12" cy="12" r="3"/>
                      </svg>
                      <span>Show password</span>
                    </>
                  )}
                </button>
              </div>
            </>
          )}

          {success && (
            <div
              className="form-success"
              role="status"
              style={{
                marginBottom: 16,
                padding: '10px 14px',
                borderRadius: 'var(--r-md)',
                background: 'rgba(74, 222, 128, 0.1)',
                border: '1px solid rgba(74, 222, 128, 0.3)',
                color: '#4ade80',
                fontSize: 13,
                display: 'flex',
                alignItems: 'center',
                gap: 8,
              }}
            >
              <span aria-hidden style={{ fontWeight: 700 }}>✓</span> {success}
            </div>
          )}

          {error && (
            <div className="form-error" role="alert" style={{ marginBottom: 16 }}>
              <span aria-hidden>⚠</span> {error}
            </div>
          )}

          {tab !== 'reset' && (
            <div className="remember-row">
              <input
                type="checkbox"
                id="remember"
                checked={remember}
                onChange={(e) => setRemember(e.target.checked)}
              />
              <label htmlFor="remember">Remember me</label>
            </div>
          )}

          <button
            type="submit"
            className="btn btn-primary btn-full"
            disabled={pending}
          >
            {pending
              ? (tab === 'signin' ? 'Signing in…' : tab === 'signup' ? 'Creating account…' : 'Resetting password…')
              : (tab === 'signin' ? 'Enter auction floor' : tab === 'signup' ? 'Create account' : 'Set new password & enter')}
          </button>

          {tab === 'reset' ? (
            <div style={{ marginTop: 14, textAlign: 'center' }}>
              <button
                type="button"
                className="btn btn-ghost btn-sm"
                style={{ color: 'var(--text-muted)', fontSize: 13 }}
                onClick={() => { setTab('signin'); setError(null); }}
              >
                &larr; Back to sign in
              </button>
            </div>
          ) : null}
        </form>

        <p style={{ marginTop: 20, fontSize: 12, color: 'var(--text-faint)', textAlign: 'center' }}>
          WALnut · The Acid Mechanics · OSDBMS 2026
        </p>
      </div>
    </div>
  );
}
