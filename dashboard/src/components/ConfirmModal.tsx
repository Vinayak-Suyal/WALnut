import { type ReactNode, useEffect } from 'react';

interface ConfirmModalProps {
  open: boolean;
  title: string;
  message: ReactNode;
  confirmLabel?: string;
  cancelLabel?: string;
  variant?: 'danger' | 'warning' | 'primary';
  loading?: boolean;
  onConfirm: () => void | Promise<void>;
  onCancel: () => void;
}

export function ConfirmModal({
  open,
  title,
  message,
  confirmLabel = 'Confirm',
  cancelLabel = 'Cancel',
  variant = 'danger',
  loading = false,
  onConfirm,
  onCancel,
}: ConfirmModalProps) {
  useEffect(() => {
    if (!open) return;
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && !loading) {
        onCancel();
      }
    };
    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [open, loading, onCancel]);

  if (!open) return null;

  const isDanger = variant === 'danger';
  const isWarning = variant === 'warning';
  const accentColor = isDanger ? 'var(--danger)' : isWarning ? '#f59e0b' : 'var(--accent)';
  const washBg = isDanger
    ? 'rgba(239, 68, 68, 0.12)'
    : isWarning
    ? 'rgba(245, 158, 11, 0.12)'
    : 'rgba(217, 119, 6, 0.12)';
  const borderColor = isDanger
    ? 'rgba(239, 68, 68, 0.35)'
    : isWarning
    ? 'rgba(245, 158, 11, 0.35)'
    : 'var(--border)';

  return (
    <div
      className="modal-backdrop"
      onClick={loading ? undefined : onCancel}
      style={{
        position: 'fixed',
        inset: 0,
        background: 'rgba(6, 8, 12, 0.82)',
        backdropFilter: 'blur(6px)',
        WebkitBackdropFilter: 'blur(6px)',
        zIndex: 9999,
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        padding: 16,
      }}
    >
      <div
        className="card"
        onClick={(e) => e.stopPropagation()}
        role="dialog"
        aria-modal="true"
        aria-labelledby="confirm-modal-title"
        style={{
          width: 'min(520px, 94vw)',
          padding: 24,
          background: 'var(--surface)',
          borderRadius: 'var(--r-lg)',
          boxShadow: '0 20px 48px -8px rgba(0, 0, 0, 0.7), 0 0 0 1px ' + borderColor,
          border: '1px solid ' + borderColor,
          display: 'flex',
          flexDirection: 'column',
          gap: 16,
          position: 'relative',
          overflow: 'hidden',
          animation: 'fade-up 180ms cubic-bezier(0.16, 1, 0.3, 1)',
        }}
      >
        {/* Top Warning Accent Bar */}
        <div
          style={{
            position: 'absolute',
            top: 0,
            left: 0,
            right: 0,
            height: 4,
            background: isDanger
              ? 'linear-gradient(90deg, #ef4444, #dc2626)'
              : isWarning
              ? 'linear-gradient(90deg, #f59e0b, #d97706)'
              : 'var(--accent)',
          }}
        />

        {/* Modal Header with Warning Icon */}
        <div style={{ display: 'flex', alignItems: 'flex-start', gap: 14, paddingTop: 4 }}>
          <div
            style={{
              width: 40,
              height: 40,
              borderRadius: '50%',
              background: washBg,
              border: `1px solid ${borderColor}`,
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              fontSize: 20,
              flexShrink: 0,
            }}
          >
            {isDanger ? '⚠️' : isWarning ? '⚡' : 'ℹ️'}
          </div>

          <div style={{ flex: 1 }}>
            <div
              style={{
                fontSize: 11,
                fontFamily: 'var(--font-num)',
                fontWeight: 700,
                textTransform: 'uppercase',
                letterSpacing: '0.08em',
                color: accentColor,
                marginBottom: 2,
              }}
            >
              {isDanger ? 'Critical Warning' : isWarning ? 'Warning Notice' : 'Confirmation Required'}
            </div>
            <h3
              id="confirm-modal-title"
              style={{
                margin: 0,
                fontSize: 18,
                fontWeight: 700,
                color: 'var(--text)',
                lineHeight: 1.3,
              }}
            >
              {title}
            </h3>
          </div>

          <button
            type="button"
            className="icon-btn"
            onClick={onCancel}
            disabled={loading}
            style={{ width: 28, height: 28, fontSize: 13, flexShrink: 0 }}
            aria-label="Close modal"
          >
            ✕
          </button>
        </div>

        {/* Warning Message Box */}
        <div
          style={{
            fontSize: 14,
            color: 'var(--text)',
            lineHeight: 1.6,
            background: washBg,
            border: `1px solid ${borderColor}`,
            borderRadius: 'var(--r-md)',
            padding: '14px 16px',
          }}
        >
          {message}
        </div>

        {/* Actions */}
        <div style={{ display: 'flex', justifyContent: 'flex-end', gap: 10, marginTop: 4 }}>
          <button
            type="button"
            className="btn btn-ghost btn-sm"
            onClick={onCancel}
            disabled={loading}
            style={{ padding: '8px 16px', height: 38 }}
          >
            {cancelLabel}
          </button>
          <button
            type="button"
            className={`btn ${isDanger ? 'btn-danger' : isWarning ? 'btn-primary' : 'btn-primary'} btn-sm`}
            onClick={onConfirm}
            disabled={loading}
            style={{
              padding: '8px 20px',
              height: 38,
              minWidth: 100,
              fontWeight: 600,
              background: isWarning ? '#d97706' : undefined,
              borderColor: isWarning ? '#b45309' : undefined,
            }}
          >
            {loading ? 'Processing…' : confirmLabel}
          </button>
        </div>
      </div>
    </div>
  );
}
