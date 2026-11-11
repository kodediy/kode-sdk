kaos/radio
==========

AO_Radio. Owns the ESP32-C5: the link, Wi-Fi and BLE.

Driver: kdrv/radio

Optional: CONFIG_KODEOS_RADIO

Docs: https://docs.kode.diy/sdk/kaos/radio

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
