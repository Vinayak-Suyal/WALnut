import axios from 'axios';
import type { Item, User, BidResponse, HostInfo, Stats } from '../types';

const api = axios.create({
  baseURL: import.meta.env.VITE_API_URL || '',
});

export async function createUser(username: string): Promise<User> {
  const { data } = await api.post('/api/users', { username });
  return data;
}

export async function createItem(title: string, startingPrice: number): Promise<Item> {
  const { data } = await api.post('/api/items', { title, starting_price: startingPrice });
  return data;
}

export async function getItems(): Promise<Item[]> {
  const { data } = await api.get('/api/items');
  return data;
}

export async function getItem(itemId: string): Promise<Item> {
  const { data } = await api.get(`/api/items/${itemId}`);
  return data;
}

export async function placeBid(itemId: string, userId: string, amount: number): Promise<BidResponse> {
  const { data } = await api.post('/api/bids', {
    item_id: itemId,
    user_id: userId,
    amount,
  });
  return data;
}

export async function getBids(itemId: string) {
  const { data } = await api.get(`/api/bids/${itemId}`);
  return data;
}

export async function getStats(): Promise<Stats> {
  const { data } = await api.get('/api/stats');
  return data;
}

export async function getHostInfo(): Promise<HostInfo> {
  const { data } = await api.get('/api/host-info');
  return data;
}
