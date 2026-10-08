import { useState, useEffect } from 'react';
import { Plus, XCircle } from 'lucide-react';
import QRCodePanel from '../components/QRCodePanel';
import StatsPanel from '../components/StatsPanel';
import { getHostInfo, createItem, getItems } from '../lib/api';
import type { HostInfo, Item } from '../types';

export default function Host() {
  const [hostInfo, setHostInfo] = useState<HostInfo | null>(null);
  const [items, setItems] = useState<Item[]>([]);
  const [creatingItems, setCreatingItems] = useState(false);
  const [hostError, setHostError] = useState('');

  // Fetch host info and items on mount
  useEffect(() => {
    getHostInfo()
      .then(setHostInfo)
      .catch(() => setHostError('Could not get host info. Is the server running?'));
    getItems()
      .then(setItems)
      .catch(() => { });
  }, []);

  const createDemoItems = async () => {
    setCreatingItems(true);
    try {
      await createItem('MacBook Pro M4', 1500);
      await createItem('iPhone 16 Pro', 999);
      await createItem('Sony WH-1000XM5', 250);
      await createItem('Steam Deck OLED', 400);
      await createItem('Mechanical Keyboard', 150);
      // Refresh item list
      const updated = await getItems();
      setItems(updated);
    } catch {
      // Items may already exist
    }
    setCreatingItems(false);
  };

  const closeAllAuctions = async () => {
    // This would call a close endpoint if available
    // For now we just refresh
    const updated = await getItems();
    setItems(updated);
  };

  return (
    <div className="min-h-screen bg-stone-925 text-stone-50">
      {/* Header */}
      <header className="border-b border-stone-700/40 px-4 py-5">
        <div className="max-w-6xl mx-auto text-center">
          <h1 className="text-2xl md:text-3xl font-bold tracking-tight">
            <span className="mr-2">🥜</span>
            WALnut Host Panel
          </h1>
          <p className="text-stone-500 text-sm mt-1">
            Presenter controls · QR code for audience · Live database stats
          </p>
        </div>
      </header>

      {/* Main Content */}
      <main className="max-w-6xl mx-auto p-4 md:p-8">
        {hostError ? (
          <div className="text-center py-16">
            <p className="text-red-400 bg-red-500/10 px-4 py-3 rounded-xl inline-block">
              {hostError}
            </p>
          </div>
        ) : (
          <div className="grid grid-cols-1 lg:grid-cols-2 gap-6">
            {/* Left: QR Code */}
            {hostInfo ? (
              <QRCodePanel hostInfo={hostInfo} />
            ) : (
              <div className="bg-stone-800 border border-stone-700/60 rounded-2xl p-8 flex items-center justify-center min-h-[400px]">
                <div className="text-center">
                  <div className="h-64 w-64 bg-stone-700/30 rounded-2xl animate-pulse mx-auto mb-4" />
                  <p className="text-stone-600 text-sm">Loading host info...</p>
                </div>
              </div>
            )}

            {/* Right: Stats + Controls */}
            <div className="space-y-6">
              {/* Stats */}
              <StatsPanel />

              {/* Controls */}
              <div className="bg-stone-800 border border-stone-700/60 rounded-2xl p-6">
                <h2 className="text-lg font-semibold text-stone-50 mb-4">
                  Auction Controls
                </h2>
                <div className="space-y-3">
                  <button
                    id="create-demo-items"
                    onClick={createDemoItems}
                    disabled={creatingItems}
                    className="
                      w-full py-3 rounded-xl font-semibold text-sm
                      bg-emerald-600 text-white
                      hover:bg-emerald-700 active:scale-[0.98]
                      disabled:opacity-50 disabled:cursor-not-allowed
                      transition-all duration-150
                      flex items-center justify-center gap-2
                    "
                  >
                    <Plus size={18} />
                    {creatingItems ? 'Creating...' : 'Create Demo Items'}
                  </button>
                  <button
                    id="close-all-auctions"
                    onClick={closeAllAuctions}
                    className="
                      w-full py-3 rounded-xl font-semibold text-sm
                      bg-red-600 text-white
                      hover:bg-red-700 active:scale-[0.98]
                      transition-all duration-150
                      flex items-center justify-center gap-2
                    "
                  >
                    <XCircle size={18} />
                    Close All Auctions
                  </button>
                </div>
              </div>

              {/* Active Items Summary */}
              {items.length > 0 && (
                <div className="bg-stone-800 border border-stone-700/60 rounded-2xl p-6">
                  <h2 className="text-lg font-semibold text-stone-50 mb-4">
                    Active Items ({items.filter(i => i.status === 'OPEN').length} open)
                  </h2>
                  <div className="space-y-2">
                    {items.map(item => (
                      <div
                        key={item.item_id}
                        className="flex items-center justify-between py-2 border-b border-stone-700/40 last:border-b-0"
                      >
                        <div className="flex items-center gap-3">
                          <span
                            className={`
                              w-2 h-2 rounded-full shrink-0
                              ${item.status === 'OPEN' ? 'bg-emerald-500' : 'bg-red-500'}
                            `}
                          />
                          <span className="text-sm text-stone-300">{item.title}</span>
                        </div>
                        <span className="font-mono text-sm font-semibold text-walnut-400">
                          ${(item.current_bid > 0 ? item.current_bid : item.starting_price).toFixed(2)}
                        </span>
                      </div>
                    ))}
                  </div>
                </div>
              )}
            </div>
          </div>
        )}
      </main>
    </div>
  );
}
