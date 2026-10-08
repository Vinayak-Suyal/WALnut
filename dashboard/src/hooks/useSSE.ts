import { useEffect, useRef } from 'react';

const API_BASE = import.meta.env.VITE_API_URL || '';

export function useSSE(eventName: string, callback: (data: Record<string, unknown>) => void) {
  const callbackRef = useRef(callback);
  callbackRef.current = callback;

  useEffect(() => {
    const source = new EventSource(`${API_BASE}/api/stream`);

    source.addEventListener(eventName, (event: MessageEvent) => {
      try {
        const data = JSON.parse(event.data);
        callbackRef.current(data);
      } catch (e) {
        console.error('SSE parse error:', e);
      }
    });

    source.onerror = () => {
      console.warn('SSE connection lost. Reconnecting...');
      // EventSource auto-reconnects by default.
    };

    return () => source.close();
  }, [eventName]);
}
