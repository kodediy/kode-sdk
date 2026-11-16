kaos/radio
==========

AO_Radio. Owns the ESP32-C5: the link, Wi-Fi and BLE.

Driver: kdrv/radio

Optional: CONFIG_KODEOS_RADIO

Docs: https://docs.kode.diy/sdk/kaos/radio

Scans
-----

One scan somebody asked for is alive per radio, running or waiting: the listing on the bus (`wifi_scan_request()`, `ble_scan_request()`, the console's `wifi_scan` and `ble_scan`) or an asked scan (`wifi_scan_ask()`, `ble_scan_ask()` and the blocking pair, which `kode_sys` and `kode.h` wrap). The newest request of a radio wins: a Wi-Fi scan, a join, Wi-Fi switched off, or on from off, end the Wi-Fi scan in flight, and a Bluetooth listen or Bluetooth switched ends the listen. A request of one radio ends no scan of the other.

The asker of the scan that ended is answered at that moment: an asked scan with `rc` `KODE_CANCELLED`, a listing with an end that says `cut`. A Wi-Fi listing ended by a newer `wifi_scan` says `WIFI_SCAN_AGAIN_SIG` instead and the new sweep's end closes it. An asker that scans again at every answer waits for its own end first, or two of them never let a sweep finish.

The two listing doors post with `KQP_RADIO_POST_MARGIN` and return false when AO_Radio's queue is that close to full. Each request takes a generation that its listing's entries and ends carry, so a listener skips what belongs to a listing before its own.

On the console a valid line from the port that asked for a scan takes the scan's place (`wifi_scan`, `wifi_join`, `wifi_down` behind `wifi_scan`; `ble_scan`, `ble_down` behind `ble_scan`), and the scan is closed first with `>err code=5 msg=stopped`.

An update of the C5's firmware, a join in flight, the board's own round, the production row and the radio check are not cut. The page linked above says what each request is answered behind them.

Wi-Fi frames
------------

The C5 hears Wi-Fi frames on one channel when asked and sends what it heard in
batches. AO_Radio offers that to one asker at a time: `wifi_frames_request(who,
on, channel)` (`kaos.h`), which `kode_sys_wifi_frames()` wraps for an app's
Active Object. It posts the request and returns; the answer comes to `who` in
every state of the link, as `WIFI_FRAMES_STATE_SIG` (`on`, `channel`, `joined`
and `why`: listening or stopped as asked, no link, a C5 without the service,
Wi-Fi off, joined, the channel refused, somebody else listening, no memory,
the board going to rest). A request AO_Radio could not send or the C5 did not
answer in time reads as no link (busy while the service's one request is still
out); a promiscuous call the C5's radio refuses reads as Wi-Fi off; no room for
this chip's ring reads as no memory.

While it listens, `who` gets `WIFI_FRAMES_TICK_SIG` for each 100 ms in which a
frame was heard (the count heard since the start, management and data frames
in the period, the strongest level and the frames lost) and, after it,
`WIFI_FRAME_SIG` for each frame handed over: its channel, level, frame
control, the two addresses, its length and the first bytes after the header.
All three are posted to the asker, never published, and the ticks and frames
leave `KQP_FRAMES_MARGIN` slots of its queue free; what does not fit is counted
in the next tick's `dropped`. Receive only: nothing is sent, and the station
never leaves its network, so joined, only channel 0 or the network's is
heard. A second asker is answered busy and its stop changes nothing. A batch
that arrives with nobody listening makes AO_Radio send one stop, which quiets a
C5 that kept listening across a reset of this chip.

`dropped` is this chip's count alone: the ring's overflow and what the asker's
queue or the pool had no room for. A frame the C5 could not queue, or one past
what a batch carries, is in `heard` and in no drop count, and a tick this chip
could not post is counted nowhere (the next one carries the totals since the
start). A tick's `channel` is the radio's when the C5 sent the batch, so frames
heard while a scan visited other channels carry the channel at the period's
end.

AO_Radio ends a stream itself, with a stop to the C5 and an answer to the asker,
before work it would not survive: a round of the board's rest to ASLEEP (busy
`frames` until the stop is answered, and a start is refused `resting` until the
board is awake), Wi-Fi stopped and every join, which may restart the station
(`WIFI_OFF`), and a C5 update (`BUSY`, and a start is refused busy while it
runs). While AO_Radio joins a network by itself, a start that names a channel is
refused busy; channel 0 is taken. A `wifi_frames` that ends with its stop never
in AO_Radio's queue leaves the console the asker: the console stops the stream at
its next tick.

The console verb `wifi_frames ch=<0..177> ms=<100..10000>` listens that long,
stops and prints one record of counts and levels, then `>ok`:
`>|frames ch= joined= heard= handed= dropped= mgmt= data= rssi_max=`.
It never prints an address or a payload.

The asker, the request in flight and the count of frames lost are in
`src/radio_frames.c`; the C5's batches are decoded by `kdrv/radio`
(`kdrv_frames`).

BLE advertisements
------------------

The host stack hears BLE advertisements while a passive scan with no end runs, which is one job of the radio worker (`kdrv_ble_hear()`): interval and window are both 10 ms, so the radio listens all the time and a BLE console goes slower meanwhile (`BLE_HEAR_WINDOW` in `kdrv_ble.c` is the one constant that makes it half). AO_Radio offers what it hears to one asker at a time: `ble_heard_request(who, on)` (`kaos.h`, in a build with `CONFIG_KODEOS_BLE`). It posts the request and returns; the answer comes to `who` in every state of the link, as `BLE_HEARD_STATE_SIG` (`on`, `why` and the totals `heard` and `dropped`). Nothing is sent and Bluetooth is never brought up: with it off in Settings or its stack down the answer is `BLE_OFF`.

While it listens, `who` gets one `BLE_HEARD_SIG` per advertisement: the address as printed, its type, the level, the kind of packet and the payload as on the air. They are posted to the asker, never published, and the first comes after the answer that says the stream is on: what the scan hears while its start is still with the worker waits in the ring. Each takes a block of the medium tier and leaves `KQP_HEARD_POOL_MARGIN` blocks of it free, and leaves half of the asker's queue free (`KQP_HEARD_MARGIN` slots at least), so a slow asker never holds the console's `config` busy; what does not fit is dropped and counted in the next one's `dropped`, which also holds what the ring of 64 records refused. Nothing asserts when the asker cannot keep up. A stop is answered with the totals once the worker has stopped the scan.

The newest Bluetooth request wins, and the stream is one more Bluetooth request beside a listen (see "Scans" above). A start ends the listen somebody asked for, running or waiting: a `ble_scan` is closed with an end that says cut, an asked listen is answered `KODE_CANCELLED`, the driver is asked to end it (`kdrv_ble_scan_abort()`), and the stream's start goes when the worker is back. Another asker's start takes the stream without stopping the radio. A newer listen ends the stream in turn, and its asker is told `CANCELLED` at the moment the listen is taken; the radio check's name find and the production row's scan do the same as their job goes to the worker (`radio_submit_req()`), since the host stack has one scan. The listen cancels the stream's scan as it starts its own. A stream that was ended does not come back by itself. A Wi-Fi request ends nothing of Bluetooth: a Wi-Fi scan runs beside the stream, which holds no job. What a listen is answered busy behind is not cut by a start either: a Wi-Fi scan somebody asked for, the board's own round, the radio check, the production row, an install and an update answer `BUSY`.

AO_Radio also ends a stream itself, with an answer to the asker, before work it would not survive: Bluetooth switched off (`BLE_OFF`), a round of the board's rest to ASLEEP or to OFF (`RESTING`; the round is busy `heard` only until the scan has stopped, and a start is refused `RESTING` until the board is awake), a C5 update (`BUSY`) and the link going down (`NO_LINK`). A scan the stack ends by itself is `REFUSED`.

A stop the worker could not take is answered `on` with `BUSY`: the scan is still on and the asker asks again. A stop asked again while the first is still being served shares its answer. A stop from an AO that does not hold the stream is answered off and changes nothing. An app asks through `kode_sys_ble_heard()`.

The console verb `ble_heard ms=<100..10000>` listens that long, stops and prints one record of counts and levels, then `>ok`: `>|ble_heard heard= handed= dropped= conn= direct= scan= nonconn= rssi_max=`. The four kind columns need not add up to `handed`: a packet of another kind is counted in `handed` alone. It never prints an address, a name or a payload, and a record with no level (`KBLE_HEARD_RSSI_NONE`) is left out of `rssi_max`. It runs as `wifi_frames` does (`radio_verbs.c` keeps the window, the stop and the watchdog's net once for both).

The asker, the hear job in flight and the count of records lost are in `src/radio_heard.c`; the ring they are kept in is `kdrv_ble_heard`.
