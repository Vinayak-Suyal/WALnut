import '@fontsource/unbounded/500.css';
import '@fontsource/instrument-sans/400.css';
import '@fontsource/instrument-sans/500.css';
import '@fontsource/instrument-sans/600.css';
import '@fontsource/instrument-sans/700.css';
import '@fontsource-variable/doto';
import '@fontsource/ibm-plex-mono/400.css';
import '@fontsource/ibm-plex-mono/500.css';
import '@fontsource/ibm-plex-mono/600.css';

import './index.css';

import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { BrowserRouter, Routes, Route, Navigate } from 'react-router-dom';

import { SessionProvider } from './context/SessionContext';
import { StreamProvider }  from './context/StreamContext';
import { ToastProvider }   from './context/ToastContext';

import { AppShell }       from './components/AppShell';
import { Gateway }        from './pages/Gateway';
import { AuctionFloor }   from './pages/AuctionFloor';
import { HostConsole }    from './pages/HostConsole';
import { WonItems }       from './pages/WonItems';
import { EngineExplorer } from './pages/EngineExplorer';
import { DatabaseAdmin }  from './pages/DatabaseAdmin';
import { UserProfile }    from './pages/UserProfile';

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <SessionProvider>
      <StreamProvider>
        <ToastProvider>
          <BrowserRouter>
            <Routes>
              {/* Gateway – unauthenticated entry point */}
              <Route path="/" element={<Gateway />} />

              {/* Protected shell – all pages that need the navbar */}
              <Route element={<AppShell />}>
                <Route path="/auction" element={<AuctionFloor />} />
                <Route path="/host"    element={<HostConsole />} />
                <Route path="/won"     element={<WonItems />} />
                <Route path="/engine"  element={<EngineExplorer />} />
                <Route path="/admin"   element={<DatabaseAdmin />} />
                <Route path="/profile" element={<UserProfile />} />
              </Route>

              {/* Fallback */}
              <Route path="*" element={<Navigate to="/" replace />} />
            </Routes>
          </BrowserRouter>
        </ToastProvider>
      </StreamProvider>
    </SessionProvider>
  </StrictMode>,
);
