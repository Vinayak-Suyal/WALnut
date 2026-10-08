import { useState, useEffect } from 'react';
import { BarChart3 } from 'lucide-react';
import { getStats } from '../lib/api';
import type { Stats } from '../types';

export default function StatsPanel() {
  const [stats, setStats] = useState<Stats | null>(null);
  const [error, setError] = useState(false);

  const fetchStats = async () => {
    try {
      const data = await getStats();
      setStats(data);
      setError(false);
    } catch {
      setError(true);
    }
  };

  useEffect(() => {
    fetchStats();
    const interval = setInterval(fetchStats, 3000); // Auto-refresh every 3s
    return () => clearInterval(interval);
  }, []);

  if (error && !stats) {
    return (
      <div className="bg-stone-800 border border-stone-700/60 rounded-2xl p-6">
        <p className="text-stone-500 text-sm">Unable to load stats. Is the server running?</p>
      </div>
    );
  }

  return (
    <div className="bg-stone-800 border border-stone-700/60 rounded-2xl p-6">
      {/* Header */}
      <div className="flex items-center gap-2 mb-5">
        <BarChart3 size={18} className="text-walnut-400" />
        <h2 className="text-lg font-semibold text-stone-50">Database Stats</h2>
      </div>

      {stats ? (
        <div className="space-y-0">
          <StatsRow label="Buffer Pool Hits" value={stats.buffer_pool.hits} />
          <StatsRow label="Buffer Pool Misses" value={stats.buffer_pool.misses} />
          <StatsRow label="Evictions" value={stats.buffer_pool.evictions} />
          <StatsRow label="Dirty Evictions" value={stats.buffer_pool.dirty_evictions} />
          <StatsRow
            label="Hit Rate"
            value={`${(stats.buffer_pool.hit_rate * 100).toFixed(1)}%`}
            highlight
          />
        </div>
      ) : (
        <div className="space-y-3">
          {[1, 2, 3, 4, 5].map((i) => (
            <div key={i} className="h-8 bg-stone-700/30 rounded-lg animate-pulse" />
          ))}
        </div>
      )}
    </div>
  );
}

function StatsRow({
  label,
  value,
  highlight = false,
}: {
  label: string;
  value: string | number;
  highlight?: boolean;
}) {
  return (
    <div className="flex items-center justify-between py-2.5 border-b border-stone-700/40 last:border-b-0">
      <span className="text-sm text-stone-400">{label}</span>
      <span
        className={`
          font-mono font-semibold text-sm
          ${highlight ? 'text-walnut-400' : 'text-stone-200'}
        `}
      >
        {value}
      </span>
    </div>
  );
}
