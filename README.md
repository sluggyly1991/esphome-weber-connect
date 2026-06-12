# esphome-weber-connect

`esphome-weber-connect` is an ESPHome external component for the Weber Connect
Smart Grilling Hub. An ESP32 connects directly to the hub over Bluetooth Low
Energy, speaks the proprietary Weber/June Saber protocol, and exposes probe
temperature, battery and state entities through ESPHome's native Home
Assistant integration.

This is not a Home Assistant add-on and contains no direct MQTT client.
ESPHome provides Wi-Fi, logging, OTA updates, entity discovery and the native
Home Assistant API.

> This project is based on protocol research and reverse engineering work from
> ProspectOre's Weber Connect Home Assistant Add-on:
> https://github.com/ProspectOre/weber-connect-home-assistant-addon

The original project is MIT licensed. Its copyright and license notice are
preserved in [LICENSE](LICENSE). This project is unofficial and is not
affiliated with or endorsed by Weber-Stephen Products LLC.

## Features

- Application-level Weber pairing with physical confirmation on the hub
- Pairing material persisted in ESPHome Preferences/NVS
- Four probe temperature sensors in degrees Celsius
- Four optional wireless-probe battery sensors
- Four probe state/type text sensors
- Connection state and appliance ID text sensors
- Pair, disconnect, phone handoff and clear-pairing buttons
- Bounds-checked Saber frame, CRC and nested TLV parsing
- Cyclic connect/listen/disconnect behavior to release the hub between reads
- Debug logging without private-key output

## Requirements

- An ESP32 supported by ESPHome. A classic ESP32 or ESP32-S3 with sufficient
  flash/RAM is recommended.
- ESPHome 2026.5 or newer
- Home Assistant with the ESPHome integration
- A Weber Connect Smart Grilling Hub
- The hub's BLE MAC address

BLE uses a meaningful amount of RAM. Avoid combining this component with
audio, voice assistant or other memory-heavy ESPHome components.

## Installation

Add this repository as a Git external component. Replace `DEIN_USER` with the
GitHub owner after publishing the repository:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/DEIN_USER/esphome-weber-connect
    components: [weber_connect_ble]
```

For local development, place this repository next to your YAML and use:

```yaml
external_components:
  - source:
      type: local
      path: ./esphome-weber-connect/components
    components: [weber_connect_ble]
```

## Complete YAML example

The complete ready-to-edit configuration is in [example.yaml](example.yaml).
The essential component setup is:

```yaml
esp32_ble_tracker:

ble_client:
  - mac_address: AA:BB:CC:DD:EE:FF
    id: weber_hub
    auto_connect: true

weber_connect_ble:
  id: weber
  ble_client_id: weber_hub
  name: "Weber Connect"
  poll_interval: 30s
  listen_duration: 8s
  phone_handoff_duration: 15min
  pairing_name: "ESPHome Weber"
  handshake_characteristic: session
  pairing_characteristic: command

sensor:
  - platform: weber_connect_ble
    probe: 1
    temperature:
      name: "Weber Probe 1 Temperature"
    battery:
      name: "Weber Probe 1 Battery"

text_sensor:
  - platform: weber_connect_ble
    probe: 1
    state:
      name: "Weber Probe 1 State"
    connection_state:
      name: "Weber Connection State"
    hub_serial_or_device_id:
      name: "Weber Hub Device ID"

button:
  - platform: weber_connect_ble
    pair:
      name: "Weber Pair"
    disconnect:
      name: "Weber Disconnect"
    use_with_phone:
      name: "Weber Use With Phone"
    clear_pairing_data:
      name: "Weber Clear Pairing Data"
```

Repeat the sensor and text-sensor blocks with `probe: 2`, `3`, and `4` as
shown in `example.yaml`.

### Configuration options

| Option | Default | Description |
| --- | --- | --- |
| `ble_client_id` | required | ESPHome BLE client for the hub MAC |
| `name` | `Weber Connect` | Name used in logs |
| `poll_interval` | `30s` | Time between connection cycles |
| `listen_duration` | `8s` | Notification listening window |
| `phone_handoff_duration` | `15min` | Reconnect delay after Use With Phone |
| `pairing_name` | `ESPHome Weber` | Companion name sent to the hub, max 32 bytes |
| `handshake_characteristic` | `session` | `session` or `command` for normal telemetry |
| `pairing_characteristic` | `command` | Characteristic used for pairing request |

`poll_interval` must be longer than `listen_duration`.

## Finding the hub MAC address

The hub must be awake and advertising. It normally stops advertising while
another client is connected.

1. Close or disconnect the Weber mobile app.
2. Power on the hub and press a button to wake it.
3. Scan with Home Assistant Bluetooth diagnostics, a phone BLE scanner such as
   nRF Connect, or an ESPHome node using `esp32_ble_tracker` with
   `logger.level: VERY_VERBOSE`.
4. Look for a name containing `Weber`, `Connect`, or `June`, or manufacturer
   ID `0x0df2` (`0x07c5` on some legacy devices).
5. Put the resulting address in `ble_client.mac_address`.

Use the public/static BLE address reported by the scanner. If the address
changes between scans, collect logs before assuming the device is unsupported.

## Pairing

1. Flash the ESP32 and add it to Home Assistant through the ESPHome integration.
2. Ensure the Weber app is disconnected from the hub.
3. Power on or wake the hub and keep it close to the ESP32.
4. Press **Weber Pair** in Home Assistant.
5. Watch the ESPHome logs. The component creates a companion ID and P-256 key,
   connects, claims the Weber session and sends the pairing request.
6. When the hub beeps, press the physical button on the hub.
7. Wait for `Pairing confirmed and saved` in the logs.
8. Check the probe entities after the next measurement cycle.

Pairing is proprietary application-level authorization, not normal BLE
bonding. Reflashing without erasing flash usually keeps NVS pairing data.
Erasing flash or pressing **Weber Clear Pairing Data** requires pairing again.

## Connection behavior

By default the component connects every 30 seconds, subscribes to Status,
Notification and Response, claims the session, sends a companion handshake,
listens for eight seconds and disconnects. Temperatures arrive in GATT
notifications, not advertisements.

The hub probably permits only one active client. **Weber Use With Phone**
actively disconnects the ESP32 and suppresses reconnects for
`phone_handoff_duration`, allowing the Weber app to connect. **Weber
Disconnect** ends only the current cycle; normal polling resumes afterward.

## Troubleshooting

### ATT MTU too small

Saber pairing frames must be written as one ATT value. The component requires
an MTU of at least 114 and checks the exact configured pairing frame. Keep the
hub close, use current ESPHome/ESP-IDF releases and shorten `pairing_name` if a
long name increases the required MTU.

### Hub is already connected

Force-close the Weber app, press **Weber Use With Phone** only when needed,
then wake or power-cycle the hub. The hub may not advertise while connected.

### No notifications

Confirm that pairing succeeded, a probe is inserted, and the listen window is
long enough. Try `listen_duration: 12s`. Debug logs show subscription counts,
handles and incoming Saber message types.

### Pairing rejected or timed out

Wake the hub before pressing Pair and press its physical button immediately
after the beep. If retries continue to fail, press **Weber Clear Pairing Data**,
power-cycle the hub and pair again.

### Poor RSSI or intermittent reconnects

Move the ESP32 closer, use a board with an external antenna, separate it from
USB 3 and Wi-Fi interference, and avoid metal enclosures. BLE and Wi-Fi share
the ESP32's 2.4 GHz radio.

### Telemetry handshake compatibility

The analyzed add-on uses `session` for normal telemetry but `command` during
pairing. If a hub firmware connects but never sends status, test:

```yaml
handshake_characteristic: command
```

## Debug logging

Use:

```yaml
logger:
  level: DEBUG
  logs:
    weber_connect_ble: VERBOSE
    esp32_ble_client: DEBUG
    ble_client: DEBUG
```

Logs include state transitions, negotiated MTU, GATT handles, frame types,
sequence numbers and parser failures. Companion private keys are never logged.

## Protocol and security

See [docs/protocol.md](docs/protocol.md) for the implemented frame and TLV
format. Pairing secrets remain in ESP32 NVS. Anyone with physical access to an
unprotected ESP32 flash may be able to extract them; enable ESP32 flash
encryption and secure boot when the deployment requires stronger protection.

## License

MIT. See [LICENSE](LICENSE). The attribution and copyright notice for the
ProspectOre source project are retained as required by that license.
