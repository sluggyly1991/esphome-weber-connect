# Changelog

## 0.1.0

First public release.

- Pairs with the Weber Connect Smart Grilling Hub directly over Bluetooth —
  fully local, no Weber cloud or account required. When the hub beeps during
  pairing, press the button on the hub to confirm.
- Built-in web panel (ingress) with one-tap **Find My Hub** pairing, live hub
  and probe status, **Use with Phone** handoff with automatic reconnect, and
  **Forget This Hub**.
- Publishes probe temperature and state sensors through MQTT discovery; a
  battery sensor is added automatically for wireless probes. Empty probe
  slots read "No probe".
- Implements the hub's BLE protocol natively: session-slot claim, protocol
  version negotiation, frame encoding/decoding, and pairing key exchange
  with a P-256 keypair stored with owner-only permissions.
- Installs from prebuilt images (aarch64, amd64) published to GitHub
  Container Registry — installs and updates complete in seconds.
