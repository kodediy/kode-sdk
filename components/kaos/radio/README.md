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
Wi-Fi off, joined, the channel refused, somebody else listening, no memory on
the C5).

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

The console verb `wifi_frames ch=<0..177> ms=<100..10000>` listens that long,
stops and prints one record of counts and levels, then `>ok`:
`>|frames ch= joined= heard= handed= dropped= mgmt= data= rssi_max=`.
It never prints an address or a payload.

The asker, the request in flight and the count of frames lost are in
`src/radio_frames.c`; the C5's batches are decoded by `kdrv/radio`
(`kdrv_frames`).
