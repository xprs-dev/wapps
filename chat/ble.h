/*
 * APRS over BLE — a thin transport over the shared BLE HAL (hal_ble_*).
 *
 * Model: connectionless broadcast. Each station advertises its latest TNC2
 * frame in BLE manufacturer data; all nearby stations scan and receive it.
 * The host's BleService shares the single adapter across wapps, so this only
 * starts/stops scanning + advertising and reads/writes frames — it never owns
 * the radio exclusively.
 */
#ifndef BLE_H
#define BLE_H

/* Begin scanning for inbound frames. */
void ble_start(void);

/* Stop scanning and clear our advertisements. */
void ble_stop(void);

/* Read one inbound record as JSON {"from":..,"rssi":..,"data":"<TNC2 frame>"}
 * into buf (NUL-terminated). Returns length, or 0 when nothing is queued. */
int ble_poll(char *buf, unsigned max);

/* Broadcast one TNC2 frame over BLE (added to the shared advertise rotation). */
void ble_send(const char *frame);

#endif /* BLE_H */
