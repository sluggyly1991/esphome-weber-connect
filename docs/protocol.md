# Weber Saber BLE protocol

This document records the subset implemented by this component. It is not an
official Weber specification.

## Attribution

This project is based on protocol research and reverse engineering work from
ProspectOre's Weber Connect Home Assistant Add-on:
https://github.com/ProspectOre/weber-connect-home-assistant-addon

The original work is MIT licensed. Its copyright and license notice are
preserved in this repository's `LICENSE` file.

## GATT layout

Service: `01014a75-6e65-81de-a4b2-940b4c6f6b69`

| Name | UUID | Use |
| --- | --- | --- |
| Status | `31014a75-6e65-81de-a4b2-940b4c6f6b69` | Notifications |
| Notification | `31024a75-6e65-81de-a4b2-940b4c6f6b69` | Notifications |
| Command | `31034a75-6e65-81de-a4b2-940b4c6f6b69` | Handshake/pairing writes |
| Session | `31044a75-6e65-81de-a4b2-940b4c6f6b69` | Session claim and telemetry handshake |
| Response | `31084a75-6e65-81de-a4b2-940b4c6f6b69` | Notifications and polling reads |

The hub expects each Saber frame in one ATT write. The component rejects an
MTU below 114 and also checks the exact frame length before every write.

## Frame format

Transport header:

```text
sequence       uint32 little-endian
payload_length uint16 little-endian
payload        Saber envelope
```

Plain/null-session envelope:

```text
AB 00 00 message_type body_length_le body crc8 54
```

The body starts with `message_version` and `message_type`. CRC is reflected
CRC-8/MAXIM-DOW with polynomial constant `0x8c` and initial value zero. The
CRC input begins at the envelope's `message_count` byte and ends after body.

## Pairing

Pairing is application-level authorization, not BLE bonding:

1. Generate a random 16-byte companion ID and NIST P-256 key pair.
2. Store the private scalar as 32 bytes and public point as `X || Y` (64 bytes).
3. Write `01` with response to Session.
4. Write handshake type `70`: companion ID plus a random 32-byte nonce.
5. On incoming type `F1`, write pairing request type `0A`: companion ID,
   public key, one-byte name length, UTF-8 name.
6. Confirm physically on the hub.
7. Incoming type `85` contains appliance ID, appliance public key and status.

Pairing status values are `0=confirmed`, `1=rejected`, and `2=timed out`.
Pairing material is stored in ESPHome Preferences/NVS. Private keys are never
written to logs.

## Status TLV

The payload of incoming type `80` uses one-byte tag and length fields. Top
level tag `4` contains one nested probe status and may occur repeatedly.

Important probe tags:

| Tag | Value |
| --- | --- |
| 1 | Zero-based probe slot |
| 10 | Probe temperature, signed int16 LE, 0.1 C |
| 12 | Session/probe state |
| 19 | Probe type; tag 4 is a legacy fallback |
| 20 | Serial number |
| 21 | SKU |
| 22 | Battery percentage |
| 23 | Segment temperature, repeatable |
| 24 | Case temperature |
| 25 | Ambient temperature |

Temperature `-32768` means unavailable. The parser rejects truncated headers,
fields that exceed their enclosing payload, bad frame lengths, bad CRCs and
bad envelope terminators.
