# Offline & Local Network Auction Feature

## 1. Overview
The goal of this feature is to allow users to participate in the WALnut live auction in two modes:
1. **Online Mode**: Users join the auction by navigating to a public URL/link.
2. **Offline/Local Mode**: The host device (running the WALnut engine and dashboard) creates a local Wi-Fi Hotspot. Users scan a QR code to join this network and directly access the dashboard locally, without needing external internet access.

---

## 2. Architecture & Components

### 2.1 The Wi-Fi Hotspot (Host Device)
To host an offline auction, the machine running the WALnut backend must broadcast a local Wi-Fi network.
- **Windows**: Use `netsh wlan set hostednetwork` or the Windows 10/11 Mobile Hotspot API to create a network.
- **Linux**: Use `hostapd` and `dnsmasq` (or NetworkManager) to create an Access Point (AP).
- **macOS**: Use the built-in Internet Sharing feature.

### 2.2 QR Code Generation
The host dashboard will feature a "Host Local Auction" button that generates a QR code. 
- The QR code can be encoded with the Wi-Fi connection details (`WIFI:S:<SSID>;T:<WEP|WPA|blank>;P:<PASSWORD>;H:<true|false|blank>;;`) so scanning it on a mobile device automatically connects to the network.
- A second QR code (or combined workflow) will provide the local IP address of the host machine (e.g., `http://192.168.137.1:8080`) so the user's browser automatically opens the WALnut dashboard.

### 2.3 Network Discovery & Routing
- The embedded REST server (`cpp-httplib`) must bind to `0.0.0.0` (all interfaces) rather than just `127.0.0.1` (localhost) so that devices on the local hotspot can connect to it.
- **mDNS / Bonjour (Optional)**: Broadcast `walnut.local` on the local network so users can navigate to `http://walnut.local:8080` instead of a raw IP address.

---

## 3. Implementation Steps (Draft)

### Phase 1: Engine Binding
1. Update `main.cpp` or the API server configuration to listen on `0.0.0.0:8080` instead of `localhost:8080`.
2. Ensure the firewall on the host machine allows incoming connections on port `8080`.

### Phase 2: QR Code Integration (Dashboard)
1. Add a library like `qrcode.react` to the React dashboard.
2. Create a "Host Control Panel" UI component.
3. Use WebRTC (`RTCPeerConnection`) or a small native helper script to detect the host machine's Local Area Network (LAN) IP address.
4. Render a QR code containing `http://<LAN_IP>:8080`.

### Phase 3: Hotspot Automation (Optional / Helper Script)
1. Write a shell/PowerShell script (`scripts/start_hotspot.ps1` or `scripts/start_hotspot.sh`) that automates creating the AP.
2. The dashboard can display instructions ("Run `scripts/start_hotspot.ps1` to start the network") along with the Wi-Fi joining QR Code.

---

## 4. User Experience Workflow
1. **Host** starts the WALnut engine and runs the hotspot script.
2. **Host** opens the dashboard and navigates to the "Invite" tab.
3. **Host** displays the screen to the audience.
4. **Bidders** pull out their smartphones and scan the Wi-Fi QR code to join the network.
5. **Bidders** scan the Dashboard QR code (or click a captive portal link) to open the bidding UI.
6. **Bidders** place bids in real-time. All traffic stays on the local network (ultra-low latency, no internet required).
