# Gateway radio co-processor (TODO)

Firmware for the EFR32FG28 chips inside the building gateway: one per data
channel plus one on the control channel. It reuses `common/slp` (frames,
slot timing) and is driven by the gateway host through
`gateway/internal/rcp`.

Responsibilities:

- keep the period/slot clock and send beacons
- transmit queued frames exactly at a lock's slot start, wait for the ACK
  (5 ms), report the outcome to the host
- receive uplinks all the time (locks transmit with CSMA) and forward them
- send asynchronous wake-up trains for locks that lost sync

The host does scheduling, queues and link-layer encryption; the
co-processor only does timing-critical radio work.
