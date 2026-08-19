# Chat's wire format on the air (for ESP32 / other peers)

Connectionless **broadcast**: each station advertises its latest frame; all
nearby stations scan and receive it. No pairing/connections.

## What chat sends: XPRS

Since 0.4.38 chat airs **XPRS** — the format specified in `docs/XPRS.md` in the
aurora repo, and the one the rest of the device already speaks. `key:value`
fields, single spaces, `t:` first, `m:` last and greedy, 250 bytes maximum:

```
t:message     f:X16JK8 d:CT1ABC ts:2026-08-08_14:26:40 m:hello
t:message     f:X16JK8 d:WX     ts:2026-08-08_14:26:40 m:Net at 8pm
t:message     f:X16JK8          ts:2026-08-08_14:26:40 m:>>anyone around?
t:observation f:X16JK8          ts:2026-08-08_14:26:40 pos:38.7223,-9.1393 m:Aurora BLE
```

(Spaces are single on the wire; they are lined up above only to read.)

The mapping from what chat routes on:

| chat | XPRS |
|---|---|
| 1:1 to a callsign | `t:message` with `d:<callsign>` |
| group `#WX` | `t:message` with `d:WX` — XPRS group names carry no `#` |
| area / geo-chat | `t:message` with no `d:` |
| position | `t:observation` with `pos:`, comment in `m:` |

A `d:` read back is a station when it looks like one (`X1`/`X3`/`X5` plus four,
an SSID, or a digit in the first three characters) and a group otherwise —
`docs/XPRS.md` section 6.3.

Chat's own conventions ride **inside `m:`** and are unchanged: an `ENC1:`
ciphertext, a `~` signature, a `+<id> ` reply marker. `m:` is greedy and last,
so spaces and colons in there are safe.

`ts:` is the SENDER's clock, which the old frame never carried: a message that
waited in a mailbox now shows the minute it was written rather than the minute
it arrived.

## The compact frame, still read and still sent for some things

The older three-field form is what chat sent before, and it remains:

```
<from> 0x1F <to> 0x1F <text>
```

- `from` — sender callsign (e.g. `X16JK8`)
- `to` — a callsign (1:1), `#GRP` (group), `!` (position, `text` =
  `lat,lon[,comment]`), or empty (area / geo-chat, may start with `>>`)

**Every receiver reads both formats**, so a peer on an older build keeps
working. Chat still SENDS the compact form for:

- the wapp-to-wapp control frames, which have no XPRS meaning yet:
  `?MAIL`, `?IGATE`, `?HELLO`, `?PING`, `?PONG`
- anything that would exceed 250 bytes — XPRS section 6.6 (`n:` parts) is the
  answer to a long message, and until that is wired the compact frame carries
  it whole over Reticulum

**ESP32 note.** A dongle built before this change still emits and expects the
compact frame. It will keep being understood, but it does not yet understand
chat's XPRS output; the firmware port is pending.

## Advertisement

Carried in **manufacturer-specific data**:

- Company ID: **0xFFFF** (reserved/test id). Must match on every device.
- BLE5 extended advertising carries the frame whole (measured ceilings: 296
  bytes on TANK2, 184 on the older tablet). On legacy advertising only ~27
  bytes fit, and longer payloads are skipped by the host.

## Behaviour

- The identical frame also rides **Reticulum** (broadcast + directed datagrams,
  tagged `RET` on receipt) — the PRIMARY transport; BLE is the local off-grid
  path and APRS-IS is legacy/opt-in (licensed callsign only).
- Frames are deduped by content across Reticulum, BLE and APRS-IS, so a station
  on several transports shows each message once.
- With "Relay Bluetooth ↔ internet" on, a dual-link station bridges: BLE→internet
  is re-originated as APRS third-party traffic (`MYCALL>APRS,TCPIP*:}<TNC2>`,
  reconstructed from the routing fields); internet→BLE re-encodes the parsed
  APRS packet.
- BLE is shared across wapps by the host (one adapter, fan-out scan +
  multiplexed advertise); chat does not own it exclusively.
- Everything chat airs over BLE now also shows up in the **XPRS wapp's Traffic
  screen**, which reads the same packets off the radio — including the ones
  addressed to other stations.
