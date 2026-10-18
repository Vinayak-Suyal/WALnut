// User Profile & Account Settings page
// Allows users to view all account details, edit their email/phone/username,
// change their password, and inspect account activity metrics.

import { useState } from 'react';
import { useSession } from '../context/SessionContext';
import { useToast } from '../context/ToastContext';
import { auth, ApiError } from '../lib/api';
import { sha256 } from '../lib/crypto';

export function UserProfile() {
  const { user, updateUser, signOut } = useSession();
  const toast = useToast();

  // Profile edit state
  const [username, setUsername] = useState(user?.username || '');
  const [email, setEmail]       = useState(user?.email || '');
  const [phone, setPhone]       = useState(user?.phone || '');
  const [savingProfile, setSavingProfile] = useState(false);
  const [profileSuccess, setProfileSuccess] = useState(false);

  // Password change state
  const [oldPassword, setOldPassword] = useState('');
  const [newPassword, setNewPassword] = useState('');
  const [confirmPassword, setConfirmPassword] = useState('');
  const [changingPassword, setChangingPassword] = useState(false);
  const [passwordError, setPasswordError] = useState<string | null>(null);
  const [passwordSuccess, setPasswordSuccess] = useState(false);

  if (!user) {
    return (
      <div style={{ textAlign: 'center', padding: '60px 20px' }}>
        <h2 style={{ fontSize: 20, marginBottom: 12 }}>Please sign in to view your profile</h2>
        <button className="btn btn-primary" onClick={() => window.location.assign('/')}>
          Go to Sign In
        </button>
      </div>
    );
  }

  async function handleSaveProfile(e: React.FormEvent) {
    e.preventDefault();
    setSavingProfile(true);
    setProfileSuccess(false);
    try {
      const updated = await auth.updateProfile({
        username: username.trim(),
        email: email.trim(),
        phone: phone.trim(),
      });
      updateUser(updated);
      toast('Profile updated successfully!', 'success');
      setProfileSuccess(true);
      setTimeout(() => setProfileSuccess(false), 3000);
    } catch (err: unknown) {
      toast(err instanceof ApiError ? err.message : 'Failed to update profile.', 'error');
    } finally {
      setSavingProfile(false);
    }
  }

  async function handleChangePassword(e: React.FormEvent) {
    e.preventDefault();
    setPasswordError(null);
    setPasswordSuccess(false);

    if (newPassword.length < 4) {
      setPasswordError('New password must be at least 4 characters long.');
      return;
    }
    if (newPassword !== confirmPassword) {
      setPasswordError('New passwords do not match.');
      return;
    }

    setChangingPassword(true);
    try {
      const oldHash = oldPassword ? await sha256(oldPassword) : undefined;
      const newHash = await sha256(newPassword);

      await auth.changePassword({
        old_password: oldHash,
        new_password: newHash,
      });

      toast('Password changed successfully!', 'success');
      setPasswordSuccess(true);
      setOldPassword('');
      setNewPassword('');
      setConfirmPassword('');
      setTimeout(() => setPasswordSuccess(false), 4000);
    } catch (err: unknown) {
      setPasswordError(err instanceof ApiError ? err.message : 'Failed to change password.');
    } finally {
      setChangingPassword(false);
    }
  }

  const initials = user.username.slice(0, 2).toUpperCase();

  return (
    <div style={{ paddingBottom: 60 }}>
      {/* Header card */}
      <div
        className="card"
        style={{
          display: 'flex',
          alignItems: 'center',
          gap: 20,
          padding: '24px',
          marginBottom: 24,
          background: 'linear-gradient(135deg, var(--surface) 0%, var(--surface-2) 100%)',
          border: '1px solid var(--border)',
        }}
      >
        <div
          style={{
            width: 64,
            height: 64,
            borderRadius: '50%',
            background: 'var(--accent)',
            color: '#000',
            fontWeight: 800,
            fontSize: 24,
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'center',
            boxShadow: '0 4px 12px rgba(245, 158, 11, 0.25)',
          }}
        >
          {initials}
        </div>
        <div style={{ flex: 1 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
            <h1 style={{ fontSize: 22, fontWeight: 700, margin: 0 }}>{user.username}</h1>
            <span
              className="badge"
              style={
                user.role === 'ADMIN'
                  ? { background: 'var(--danger-wash)', color: 'var(--danger)', borderColor: 'var(--danger)' }
                  : { background: 'var(--surface-2)', color: 'var(--text-muted)' }
              }
            >
              {user.role || 'USER'}
            </span>
          </div>
          <div style={{ fontSize: 13, color: 'var(--text-muted)', marginTop: 4 }}>
            User ID: <code style={{ fontFamily: 'var(--font-num)', color: 'var(--text)' }}>{user.user_id}</code>
          </div>
        </div>
        <button className="btn btn-ghost btn-sm" onClick={signOut} style={{ color: 'var(--danger)' }}>
          Sign out
        </button>
      </div>

      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(360px, 1fr))', gap: 24 }}>
        {/* Profile Information & Edit Form */}
        <div className="card" style={{ padding: 24 }}>
          <h2 className="section-title" style={{ fontSize: 16, marginBottom: 16 }}>
            Account details & information
          </h2>
          <form onSubmit={handleSaveProfile}>
            <div className="field">
              <label className="field-label" htmlFor="prof-uid">Account ID (Immutable)</label>
              <input
                id="prof-uid"
                className="field-input"
                value={user.user_id}
                disabled
                style={{ opacity: 0.6, cursor: 'not-allowed', fontFamily: 'var(--font-num)' }}
              />
            </div>

            <div className="field">
              <label className="field-label" htmlFor="prof-uname">Display username</label>
              <input
                id="prof-uname"
                className="field-input"
                value={username}
                onChange={(e) => setUsername(e.target.value)}
                required
                disabled={savingProfile}
              />
            </div>

            <div className="field">
              <label className="field-label" htmlFor="prof-email">Email address</label>
              <input
                id="prof-email"
                type="email"
                className="field-input"
                value={email}
                onChange={(e) => setEmail(e.target.value)}
                placeholder="user@example.com"
                disabled={savingProfile}
              />
            </div>

            <div className="field">
              <label className="field-label" htmlFor="prof-phone">Phone number</label>
              <input
                id="prof-phone"
                type="tel"
                className="field-input"
                value={phone}
                onChange={(e) => setPhone(e.target.value)}
                placeholder="+1 555-0199"
                disabled={savingProfile}
              />
            </div>

            <div className="field">
              <label className="field-label">Assigned security role</label>
              <input
                className="field-input"
                value={user.role === 'ADMIN' ? 'Administrator (Full Access)' : 'Standard Bidder & Host'}
                disabled
                style={{ opacity: 0.6, cursor: 'not-allowed' }}
              />
            </div>

            {profileSuccess && (
              <div style={{ padding: '8px 12px', background: 'rgba(74, 222, 128, 0.15)', color: '#4ade80', borderRadius: 6, fontSize: 13, marginBottom: 12 }}>
                ✓ Profile details saved to database.
              </div>
            )}

            <button type="submit" className="btn btn-primary btn-full" disabled={savingProfile}>
              {savingProfile ? 'Saving changes…' : 'Save profile details'}
            </button>
          </form>
        </div>

        {/* Change Password Form */}
        <div className="card" style={{ padding: 24 }}>
          <h2 className="section-title" style={{ fontSize: 16, marginBottom: 16 }}>
            Security & password update
          </h2>
          <form onSubmit={handleChangePassword}>
            <div className="field">
              <label className="field-label" htmlFor="pw-old">Current password</label>
              <input
                id="pw-old"
                type="password"
                className="field-input"
                value={oldPassword}
                onChange={(e) => setOldPassword(e.target.value)}
                placeholder="Leave blank if account was created without one"
                disabled={changingPassword}
              />
            </div>

            <div className="field">
              <label className="field-label" htmlFor="pw-new">New password</label>
              <input
                id="pw-new"
                type="password"
                className="field-input"
                value={newPassword}
                onChange={(e) => setNewPassword(e.target.value)}
                placeholder="At least 4 characters"
                required
                disabled={changingPassword}
              />
            </div>

            <div className="field">
              <label className="field-label" htmlFor="pw-confirm">Confirm new password</label>
              <input
                id="pw-confirm"
                type="password"
                className="field-input"
                value={confirmPassword}
                onChange={(e) => setConfirmPassword(e.target.value)}
                placeholder="Repeat new password"
                required
                disabled={changingPassword}
              />
            </div>

            {passwordError && (
              <div className="guard-msg" role="alert" style={{ marginBottom: 12 }}>
                <span aria-hidden>⚠</span>
                {passwordError}
              </div>
            )}

            {passwordSuccess && (
              <div style={{ padding: '8px 12px', background: 'rgba(74, 222, 128, 0.15)', color: '#4ade80', borderRadius: 6, fontSize: 13, marginBottom: 12 }}>
                ✓ Password changed successfully!
              </div>
            )}

            <button type="submit" className="btn btn-primary btn-full" disabled={changingPassword}>
              {changingPassword ? 'Updating password…' : 'Update password'}
            </button>
          </form>
        </div>
      </div>
    </div>
  );
}
