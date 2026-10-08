import { useState, useCallback } from 'react';

interface User {
  user_id: string;
  username: string;
}

export function useAuth() {
  const [user, setUser] = useState<User | null>(() => {
    const id = localStorage.getItem('walnut_user_id');
    const name = localStorage.getItem('walnut_username');
    return id && name ? { user_id: id, username: name } : null;
  });

  const login = useCallback((user_id: string, username: string) => {
    localStorage.setItem('walnut_user_id', user_id);
    localStorage.setItem('walnut_username', username);
    setUser({ user_id, username });
  }, []);

  const logout = useCallback(() => {
    localStorage.removeItem('walnut_user_id');
    localStorage.removeItem('walnut_username');
    setUser(null);
  }, []);

  return { user, login, logout, isAuthenticated: !!user };
}
