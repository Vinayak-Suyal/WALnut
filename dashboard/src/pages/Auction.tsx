import { useState, useEffect, useCallback } from 'react';
import { useNavigate } from 'react-router-dom';
import { AnimatePresence } from 'framer-motion';
import { LogOut } from 'lucide-react';
import { useAuth } from '../hooks/useAuth';
import { useSSE } from '../hooks/useSSE';
import { getItems } from '../lib/api';
import ItemCard from '../components/ItemCard';
import BidModal from '../components/BidModal';
import type { Item } from '../types';

export default function Auction() {
  const [items, setItems] = useState<Item[]>([]);
  const [selectedItem, setSelectedItem] = useState<Item | null>(null);
  const [flashingId, setFlashingId] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const { user, logout, isAuthenticated } = useAuth();
  const navigate = useNavigate();

  // Redirect if not logged in
  useEffect(() => {
    if (!isAuthenticated) {
      navigate('/', { replace: true });
    }
  }, [isAuthenticated, navigate]);

  // Fetch initial items
  useEffect(() => {
    getItems()
      .then(setItems)
      .catch(() => {})
      .finally(() => setLoading(false));
  }, []);

  // Listen for real-time bid updates via SSE
  const handleBidUpdate = useCallback((data: Record<string, unknown>) => {
    const itemId = data.item_id as string;
    const amount = data.amount as number;
    const currentWinner = data.current_winner as string;

    setItems(prev =>
      prev.map(item =>
        item.item_id === itemId
          ? { ...item, current_bid: amount, current_winner: currentWinner }
          : item
      )
    );

    // Flash the updated card briefly
    setFlashingId(itemId);
    setTimeout(() => setFlashingId(null), 600);
  }, []);

  useSSE('new_bid', handleBidUpdate);

  const handleLogout = () => {
    logout();
    navigate('/');
  };

  if (!isAuthenticated) return null;

  return (
    <div className="min-h-screen bg-stone-925 text-stone-50">
      {/* Header */}
      <header className="sticky top-0 z-40 bg-stone-925/90 backdrop-blur-sm border-b border-stone-700/40">
        <div className="max-w-6xl mx-auto flex justify-between items-center px-4 py-3">
          <h1 className="text-lg font-bold tracking-tight">
            <span className="mr-1.5">🥜</span>
            WALnut
          </h1>
          <div className="flex items-center gap-4">
            <span className="text-walnut-400 text-sm font-medium">
              {user?.username}
            </span>
            <button
              onClick={handleLogout}
              className="text-stone-600 hover:text-stone-400 transition-colors"
              title="Sign out"
            >
              <LogOut size={18} />
            </button>
          </div>
        </div>
      </header>

      {/* Item Grid */}
      <main className="max-w-6xl mx-auto p-4">
        {loading ? (
          /* Loading skeleton */
          <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
            {[1, 2, 3].map((i) => (
              <div
                key={i}
                className="bg-stone-800 border border-stone-700/60 rounded-2xl p-6 space-y-4"
              >
                <div className="flex justify-between">
                  <div className="h-5 w-32 bg-stone-700/40 rounded-lg animate-pulse" />
                  <div className="h-5 w-14 bg-stone-700/40 rounded-full animate-pulse" />
                </div>
                <div className="h-4 w-24 bg-stone-700/30 rounded animate-pulse" />
                <div className="h-8 w-28 bg-stone-700/40 rounded-lg animate-pulse" />
                <div className="h-10 w-full bg-stone-700/30 rounded-xl animate-pulse" />
              </div>
            ))}
          </div>
        ) : items.length === 0 ? (
          /* Empty state */
          <div className="text-center py-20">
            <div className="text-5xl mb-4">🥜</div>
            <h2 className="text-xl font-semibold text-stone-300 mb-2">
              No items yet
            </h2>
            <p className="text-stone-600 text-sm">
              The host hasn't added any auction items. Hang tight!
            </p>
          </div>
        ) : (
          /* Item cards */
          <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4">
            <AnimatePresence>
              {items.map(item => (
                <ItemCard
                  key={item.item_id}
                  item={item}
                  onBid={() => setSelectedItem(item)}
                  isFlashing={flashingId === item.item_id}
                />
              ))}
            </AnimatePresence>
          </div>
        )}
      </main>

      {/* Bid Modal */}
      {selectedItem && (
        <BidModal
          item={selectedItem}
          onClose={() => setSelectedItem(null)}
          onBidPlaced={(updatedItem) => {
            setItems(prev =>
              prev.map(i =>
                i.item_id === updatedItem.item_id ? updatedItem : i
              )
            );
            setSelectedItem(null);
          }}
        />
      )}
    </div>
  );
}
