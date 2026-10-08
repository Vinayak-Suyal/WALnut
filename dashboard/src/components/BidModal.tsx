import { useState } from 'react';
import { motion, AnimatePresence } from 'framer-motion';
import { X } from 'lucide-react';
import { useAuth } from '../hooks/useAuth';
import { placeBid } from '../lib/api';
import type { Item } from '../types';

interface Props {
  item: Item;
  onClose: () => void;
  onBidPlaced: (updatedItem: Item) => void;
}

export default function BidModal({ item, onClose, onBidPlaced }: Props) {
  const { user } = useAuth();
  const [amount, setAmount] = useState('');
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');

  const currentBid = item.current_bid > 0 ? item.current_bid : item.starting_price;
  const minBid = currentBid + 1;

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!user) return;

    const numAmount = parseFloat(amount);
    if (isNaN(numAmount) || numAmount <= currentBid) {
      setError(`Bid must be higher than $${currentBid.toFixed(2)}`);
      return;
    }

    setLoading(true);
    setError('');
    try {
      const result = await placeBid(item.item_id, user.user_id, numAmount);
      onBidPlaced(result.item);
    } catch (err: unknown) {
      const axiosErr = err as { response?: { data?: { error?: string } } };
      setError(axiosErr.response?.data?.error || 'Failed to place bid');
    } finally {
      setLoading(false);
    }
  };

  return (
    <AnimatePresence>
      <div className="fixed inset-0 z-50 flex items-center justify-center p-4">
        {/* Backdrop */}
        <motion.div
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          exit={{ opacity: 0 }}
          transition={{ duration: 0.15 }}
          className="absolute inset-0 bg-stone-950/80"
          onClick={onClose}
        />

        {/* Modal */}
        <motion.div
          initial={{ opacity: 0, y: 8 }}
          animate={{ opacity: 1, y: 0 }}
          exit={{ opacity: 0, y: 8 }}
          transition={{ duration: 0.2, ease: 'easeOut' }}
          className="
            relative w-full max-w-md
            bg-stone-800 border border-stone-700/60
            rounded-2xl p-6 z-10
          "
        >
          {/* Close button */}
          <button
            onClick={onClose}
            className="absolute top-4 right-4 text-stone-500 hover:text-stone-300 transition-colors"
          >
            <X size={20} />
          </button>

          {/* Header */}
          <h2 className="text-xl font-bold text-stone-50 mb-1">Place Bid</h2>
          <p className="text-stone-400 text-sm mb-4">{item.title}</p>

          {/* Current Bid */}
          <div className="bg-stone-900/60 rounded-xl p-4 mb-5 border border-stone-700/40">
            <p className="text-stone-500 text-xs uppercase tracking-wider font-medium mb-1">
              Current Bid
            </p>
            <p className="text-2xl font-bold font-mono text-walnut-400">
              ${currentBid.toFixed(2)}
            </p>
            {item.current_winner && (
              <p className="text-xs text-stone-500 mt-1">
                by {item.current_winner}
              </p>
            )}
          </div>

          {/* Bid Form */}
          <form onSubmit={handleSubmit} className="space-y-4">
            <div>
              <label className="block text-sm font-medium text-stone-300 mb-2">
                Your Bid (minimum ${minBid.toFixed(2)})
              </label>
              <div className="relative">
                <span className="absolute left-4 top-1/2 -translate-y-1/2 text-stone-500 font-mono font-semibold">
                  $
                </span>
                <input
                  type="number"
                  step="0.01"
                  min={minBid}
                  value={amount}
                  onChange={(e) => setAmount(e.target.value)}
                  placeholder={minBid.toFixed(2)}
                  autoFocus
                  className="
                    w-full pl-9 pr-4 py-3 rounded-xl
                    bg-stone-900/80 border border-stone-700/60
                    text-stone-50 font-mono text-lg
                    placeholder-stone-600
                    focus:outline-none focus:border-walnut-500 focus:ring-2 focus:ring-walnut-500/20
                    transition-colors duration-150
                  "
                />
              </div>
            </div>

            {/* Error */}
            {error && (
              <p className="text-red-400 text-sm bg-red-500/10 px-3 py-2 rounded-lg">
                {error}
              </p>
            )}

            {/* Actions */}
            <div className="flex gap-3 pt-1">
              <button
                type="button"
                onClick={onClose}
                className="
                  flex-1 py-2.5 rounded-xl font-semibold text-sm
                  border border-stone-600 text-stone-300
                  hover:bg-stone-700/50
                  transition-colors duration-150
                "
              >
                Cancel
              </button>
              <button
                type="submit"
                disabled={loading || !amount}
                className="
                  flex-1 py-2.5 rounded-xl font-semibold text-sm
                  bg-walnut-600 text-white
                  hover:bg-walnut-700 active:scale-[0.98]
                  disabled:opacity-50 disabled:cursor-not-allowed
                  transition-all duration-150
                "
              >
                {loading ? 'Placing...' : 'Submit Bid'}
              </button>
            </div>
          </form>
        </motion.div>
      </div>
    </AnimatePresence>
  );
}
