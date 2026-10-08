export interface User {
  user_id: string;
  username: string;
}

export interface Item {
  item_id: string;
  title: string;
  starting_price: number;
  status: 'OPEN' | 'CLOSED';
  current_bid: number;
  current_winner: string;
}

export interface Bid {
  bid_id: number;
  item_id: string;
  user_id: string;
  amount: number;
  bid_time: string;
}

export interface BidResponse {
  bid_id: number;
  item_id: string;
  user_id: string;
  amount: number;
  item: Item;
}

export interface HostInfo {
  lan_ip: string;
  port: number;
  dashboard_url: string;
  hostname: string;
}

export interface Stats {
  buffer_pool: {
    hits: number;
    misses: number;
    evictions: number;
    dirty_evictions: number;
    hit_rate: number;
  };
}
