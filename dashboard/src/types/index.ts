// WALnut – shared TypeScript types
// Mirrors the C++ server's JSON shapes exactly.

export type Category = 'ELECTRONICS' | 'ART' | 'WATCHES' | 'COLLECTIBLES' | 'MUSIC' | 'GENERAL';
export type Condition = 'NEW' | 'LIKE_NEW' | 'GOOD' | 'FAIR' | 'MINT' | 'EXCELLENT';
export type LotStatus = 'OPEN' | 'CLOSED' | 'CANCELLED' | 'UNSOLD';

export interface TopBid {
  bid_id: number;
  user_id: string;
  amount: number;
  timestamp_ms: number;
}

/** Raw shape returned by /api/items */
export interface RawLot {
  item_id: string;
  host_id: string;
  title: string;
  category: string;
  condition: string;
  starting_price: number;
  status: string;
  current_bid: number;
  current_winner: string;
  end_time_ms: number;
  top_bid_count: number;
  total_bid_count: number;
  image_urls: string[];
  top_bids: TopBid[];
}

/** Normalised lot used throughout the UI */
export interface Lot {
  id: string;
  title: string;
  category: Category;
  condition: Condition;
  host: string;
  images: string[];
  startingPrice: number;
  currentBid: number | null;
  currentWinner: string | null;
  minIncrement: number;
  podium: PodiumEntry[];
  status: LotStatus;
  endsAt: string;       // ISO string derived from end_time_ms
  totalBids: number;
}

export interface PodiumEntry {
  amount: number;
  user: string;
  at: string; // ISO timestamp
}

export interface PlaceBidRequest {
  item_id: string;
  user_id: string;
  amount: number;
}

export type BidRejection =
  | 'HOST_CANNOT_BID'
  | 'ALREADY_LEADING'
  | 'BELOW_MINIMUM'
  | 'OUTBID_RACE'
  | 'LOT_CLOSED'
  | 'UNKNOWN';

export interface PlaceBidResponse {
  ok: boolean;
  bid_id?: number;
  item_id?: string;
  user_id?: string;
  amount?: number;
  item?: RawLot;
  lsn?: string;
  txId?: string;
  extended?: boolean;
  newEndsAt?: string;
  minNext?: number;
  reason?: BidRejection;
  message?: string;
  error?: string;
}

export interface CreateLotRequest {
  title: string;
  category: string;
  condition: string;
  starting_price: number;
  end_time_ms: number;
  image_urls: string[];
  host_id: string;
}

export interface HostInfo {
  lan_ip: string;
  port: number;
  dashboard_url: string;
  hostname: string;
}

export interface LedgerEntry {
  lsn: string;
  txId: string;
  op: 'BID' | 'CREATE_ITEM' | 'CLOSE_ITEM';
  status: 'COMMITTED' | 'ABORTED';
  at: string;
}

export interface Telemetry {
  recent: LedgerEntry[];
  bufferPool: {
    hitRatio: number;
    dirtyPages: number;
    pages?: ('clean' | 'dirty' | 'pinned')[];
    hits: number;
    misses: number;
    evictions: number;
  };
  locks: { active: number; waiting: number };
  checkpoint: { lastAt: string | null; status: string };
  storage?: { total_pages: number };
}

export type StreamEventType = 'new_bid' | 'new_item' | 'item_deleted' | 'user_deleted' | 'db_reset';

export interface StreamBidEvent {
  type: 'new_bid';
  item_id: string;
  amount: number;
  current_winner: string;
  bid_id: number;
  title?: string;
  item?: RawLot;
}

export interface StreamItemEvent {
  type: 'new_item';
  item_id: string;
  [key: string]: unknown;
}

export interface StreamItemDeletedEvent {
  type: 'item_deleted';
  item_id: string;
}

export interface StreamUserDeletedEvent {
  type: 'user_deleted';
  user_id: string;
}

export interface StreamDbResetEvent {
  type: 'db_reset';
  mode: 'purge_items' | 'factory_reset';
}

export interface StreamItemClosedEvent {
  type: 'item_closed';
  item_id?: string;
  [key: string]: unknown;
}

export type StreamEvent =
  | StreamBidEvent
  | StreamItemEvent
  | StreamItemDeletedEvent
  | StreamItemClosedEvent
  | StreamUserDeletedEvent
  | StreamDbResetEvent;

export interface SessionUser {
  user_id: string;
  username: string;
  email: string;
  phone?: string;
  role?: string;
}

/** Utility: convert RawLot → Lot */
export function normaliseLot(raw: RawLot): Lot {
  const endsAt = raw.end_time_ms
    ? new Date(raw.end_time_ms).toISOString()
    : new Date(Date.now() + 3600_000).toISOString();

  const podium: PodiumEntry[] = (raw.top_bids ?? []).slice(0, raw.top_bid_count ?? 0).map((tb) => ({
    amount: tb.amount,
    user: tb.user_id,
    at: new Date(tb.timestamp_ms).toISOString(),
  }));

  return {
    id: raw.item_id,
    title: raw.title,
    category: raw.category as Category,
    condition: raw.condition as Condition,
    host: raw.host_id,
    images: (raw.image_urls ?? []).filter(Boolean),
    startingPrice: raw.starting_price,
    currentBid: raw.current_bid > 0 ? raw.current_bid : null,
    currentWinner: raw.current_winner || null,
    minIncrement: Math.max(1, Math.ceil(raw.starting_price * 0.05)),
    podium,
    status: raw.status as LotStatus,
    endsAt,
    totalBids: raw.total_bid_count ?? 0,
  };
}
