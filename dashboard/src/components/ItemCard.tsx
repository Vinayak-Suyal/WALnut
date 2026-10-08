import { useState } from 'react';
import { motion } from 'framer-motion';
import type { Item } from '../types';

interface Props {
  item: Item;
  onBid: () => void;
  isFlashing?: boolean;
}

export default function ItemCard({ item, onBid, isFlashing }: Props) {
  const isOpen = item.status === 'OPEN';
  const [isHovered, setIsHovered] = useState(false);

  const displayBid = item.current_bid > 0 ? item.current_bid : item.starting_price;

  return (
    <motion.div
      layout
      initial={{ opacity: 0, y: 12 }}
      animate={{ opacity: 1, y: 0 }}
      transition={{ duration: 0.2, ease: 'easeOut' }}
      onMouseEnter={() => setIsHovered(true)}
      onMouseLeave={() => setIsHovered(false)}
      className={`
        rounded-2xl p-6 border transition-colors duration-150
        ${isFlashing
          ? 'border-emerald-500 bg-emerald-500/5'
          : isHovered
            ? 'border-walnut-600/50 bg-stone-800/80'
            : 'border-stone-700/60 bg-stone-800'
        }
      `}
    >
      {/* Title Row */}
      <div className="flex justify-between items-start mb-4">
        <h3 className="text-lg font-semibold text-stone-50 leading-tight pr-3">
          {item.title}
        </h3>
        <span
          className={`
            shrink-0 px-2.5 py-1 rounded-full text-xs font-semibold uppercase tracking-wide
            ${isOpen
              ? 'bg-emerald-600/15 text-emerald-400'
              : 'bg-red-600/15 text-red-400'
            }
          `}
        >
          {item.status}
        </span>
      </div>

      {/* Price Info */}
      <div className="space-y-1.5 mb-5">
        <p className="text-stone-500 text-sm">
          Starting: ${item.starting_price.toFixed(2)}
        </p>
        <p className="text-2xl font-bold font-mono text-walnut-400 tracking-tight">
          ${displayBid.toFixed(2)}
        </p>
        {item.current_winner && (
          <p className="text-sm text-stone-500">
            Leading: <span className="text-stone-400">{item.current_winner}</span>
          </p>
        )}
      </div>

      {/* Bid Button */}
      {isOpen && (
        <button
          onClick={onBid}
          className="
            w-full py-2.5 rounded-xl font-semibold text-sm
            bg-walnut-600 text-white
            hover:bg-walnut-700 active:scale-[0.98]
            transition-all duration-150
            focus-ring
          "
        >
          Place Bid
        </button>
      )}

      {!isOpen && (
        <div className="w-full py-2.5 rounded-xl text-center text-sm font-medium text-stone-600">
          Auction Closed
        </div>
      )}
    </motion.div>
  );
}
