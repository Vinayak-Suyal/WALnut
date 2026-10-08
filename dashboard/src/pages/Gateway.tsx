import { useState } from 'react';
import { useNavigate } from 'react-router-dom';
import { motion } from 'framer-motion';
import { useAuth } from '../hooks/useAuth';
import { createUser } from '../lib/api';

export default function Gateway() {
  const [username, setUsername] = useState('');
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');
  const navigate = useNavigate();
  const { login, isAuthenticated } = useAuth();

  // If already logged in, redirect to auction
  if (isAuthenticated) {
    navigate('/auction', { replace: true });
  }

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    const trimmed = username.trim();
    if (!trimmed) return;

    setLoading(true);
    setError('');
    try {
      const user = await createUser(trimmed);
      login(user.user_id, user.username);
      navigate('/auction');
    } catch (err: unknown) {
      const axiosErr = err as { response?: { data?: { error?: string } } };
      setError(axiosErr.response?.data?.error || 'Failed to register. Is the server running?');
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="min-h-screen bg-stone-925 flex items-center justify-center p-4">
      <motion.div
        initial={{ opacity: 0, y: 16 }}
        animate={{ opacity: 1, y: 0 }}
        transition={{ duration: 0.25, ease: 'easeOut' }}
        className="w-full max-w-sm"
      >
        {/* Card */}
        <div className="bg-stone-800 border border-stone-700/60 rounded-2xl p-8">
          {/* Logo & Title */}
          <div className="text-center mb-8">
            <div className="text-5xl mb-3">🥜</div>
            <h1 className="text-3xl font-bold text-stone-50 tracking-tight">
              WALnut
            </h1>
            <p className="text-stone-500 text-sm mt-1.5">
              Live Auction Engine
            </p>
          </div>

          {/* Form */}
          <form onSubmit={handleSubmit} className="space-y-4">
            <div>
              <input
                id="username-input"
                type="text"
                value={username}
                onChange={(e) => setUsername(e.target.value)}
                placeholder="Enter your name to join"
                maxLength={127}
                autoFocus
                autoComplete="off"
                className="
                  w-full px-4 py-3 rounded-xl
                  bg-stone-900/80 border border-stone-700/60
                  text-stone-50 text-base
                  placeholder-stone-600
                  focus:outline-none focus:border-walnut-500 focus:ring-2 focus:ring-walnut-500/20
                  transition-colors duration-150
                "
              />
            </div>

            {/* Error */}
            {error && (
              <motion.p
                initial={{ opacity: 0, y: -4 }}
                animate={{ opacity: 1, y: 0 }}
                className="text-red-400 text-sm bg-red-500/10 px-3 py-2 rounded-lg"
              >
                {error}
              </motion.p>
            )}

            {/* Submit */}
            <button
              id="join-button"
              type="submit"
              disabled={loading || !username.trim()}
              className="
                w-full py-3 rounded-xl font-semibold
                bg-walnut-600 text-white
                hover:bg-walnut-700 active:scale-[0.98]
                disabled:opacity-50 disabled:cursor-not-allowed
                transition-all duration-150
              "
            >
              {loading ? (
                <span className="flex items-center justify-center gap-2">
                  <svg className="animate-spin h-4 w-4" viewBox="0 0 24 24">
                    <circle
                      className="opacity-25"
                      cx="12" cy="12" r="10"
                      stroke="currentColor" strokeWidth="4" fill="none"
                    />
                    <path
                      className="opacity-75"
                      fill="currentColor"
                      d="M4 12a8 8 0 018-8V0C5.373 0 0 5.373 0 12h4z"
                    />
                  </svg>
                  Joining...
                </span>
              ) : (
                'Join Auction'
              )}
            </button>
          </form>
        </div>

        {/* Footer */}
        <p className="text-center text-stone-700 text-xs mt-6">
          WALnut · Concurrent Auction Engine · T038
        </p>
      </motion.div>
    </div>
  );
}
