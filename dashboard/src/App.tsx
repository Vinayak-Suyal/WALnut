import { BrowserRouter, Routes, Route } from 'react-router-dom';
import Gateway from './pages/Gateway';
import Auction from './pages/Auction';
import Host from './pages/Host';

export default function App() {
  return (
    <BrowserRouter>
      <Routes>
        <Route path="/" element={<Gateway />} />
        <Route path="/auction" element={<Auction />} />
        <Route path="/host" element={<Host />} />
      </Routes>
    </BrowserRouter>
  );
}
