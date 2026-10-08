import { QRCodeSVG } from 'qrcode.react';
import { QrCode } from 'lucide-react';
import type { HostInfo } from '../types';

interface Props {
  hostInfo: HostInfo;
}

export default function QRCodePanel({ hostInfo }: Props) {
  return (
    <div className="bg-stone-800 border border-stone-700/60 rounded-2xl p-8 flex flex-col items-center">
      {/* Header */}
      <div className="flex items-center gap-2 mb-6">
        <QrCode size={20} className="text-walnut-400" />
        <h2 className="text-xl font-semibold text-stone-50">Scan to Join</h2>
      </div>

      {/* QR Code with white container */}
      <div className="bg-white p-5 rounded-2xl shadow-lg">
        <QRCodeSVG
          value={hostInfo.dashboard_url}
          size={280}
          level="H"
          includeMargin={false}
          bgColor="#FFFFFF"
          fgColor="#1C1917"
        />
      </div>

      {/* URL */}
      <p className="mt-5 font-mono text-walnut-400 text-base font-semibold select-all">
        {hostInfo.dashboard_url}
      </p>

      {/* Instructions */}
      <div className="mt-3 text-center space-y-1">
        <p className="text-stone-500 text-sm">
          Connect to the Wi-Fi hotspot first, then scan
        </p>
        <p className="text-stone-600 text-xs font-mono">
          Host: {hostInfo.hostname} · LAN: {hostInfo.lan_ip}
        </p>
      </div>
    </div>
  );
}
