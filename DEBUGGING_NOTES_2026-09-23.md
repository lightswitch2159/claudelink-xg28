---
record_type: hardware_debugging_handoff
date: 2026-09-23
project: OrangeLink xG28 / Medtronic 722 radio link
status: resolved
active_pump_serial: "646910"
active_pump_role: bench
receive_only_pump_serial: "560793"
receive_only_pump_role: real pump; passive listening is allowed, transmitting is forbidden
---

# Bench pump communication investigation

## Goal and operating constraint

Find why the xG28 bridge sends Minimed radio requests but does not report a
pump reply. All active pump communication in this investigation must target
bench pump serial **646910**. Serial **560793** is a real pump and must never be
sent active requests. Receive-only listening to 560793 is allowed. Both pumps
are alive, and the user says their command protocol is the same.

## Current conclusion (updated, Follow-up 12)

**Resolved.** As of 2026-09-24, the bridge cleanly and repeatably receives and
correctly decodes bench pump 646910's real over-the-air reply and delivers it
to the host over BLE -- two full, independent, ground-truth-confirmed test
runs (`bench_646910_errrssi_test_20260924.cs8` and
`bench_646910_errrssi_confirm5_freshble_20260924.cs8`), each with the identical
71-byte, CRC-valid `a76469108d0903373232...0009` frame reported at all 5 scan
positions in stage 2. See Follow-up 12 for the full account, including a
separate host-side BLE caching gotcha (not a firmware bug) that produced three
misleading intermediate failures during reproducibility testing.

The paragraphs below (pre-2026-09-24) describe the investigation that led here
and are kept for history; they no longer describe the current state.

The bridge has emitted radio traffic during bench-targeted probes, but no
valid frame from bench serial 646910 has been delivered to the host. However,
a simultaneous HackRF capture during the latest bench tuneup contains 18
CRC-valid 71-byte model replies from 646910 (`722`) across the wake and scan
periods. The pump is responding over the air; the current bridge receive path
is missing those responses. A separate passive receive-only listen at requested
916.650 MHz did decode a CRC-valid frame from 560793, so RX is not universally
dead. The remaining fault is in tuned receive alignment, RAIL RX operation
during send-and-listen, or frame handling under this response signal.

HackRF FFT measurements show the bridge's carrier follows every 50 kHz scan
step correctly. Measured carriers are about 20.6 kHz below the requested
frequencies, consistently, including the initial 916.625 MHz tune. The
step-to-step map is therefore supported, but the absolute carrier calibration
is not settled. The sibling
RFM69 implementation independently writes FRF `0xE52312` for 916.548 MHz and
reports a hardware frequency of 916,547,973 Hz; its migration notes also report
a HackRF carrier measurement of 916.5477 MHz. This supports the intended
916.548 MHz nominal target and the older radio path. It does not establish
whether the xG28/HackRF ~20.6 kHz difference comes from the xG28 synthesizer or
the HackRF reference because there is no same-session calibrated reference.
A 25-second receive window returned CRC-valid bytes
carrying serial **560793**, but that foreign 0x8D frame had no model payload and
was not the expected 0x06 ACK. It is explicitly excluded as a bench response.
Current BLE probes produced no valid frame from 646910 even though HackRF
recorded the pump's valid replies during the same run. A broad AndroidAPS-style
scan of 916.45–916.80 MHz in 50 kHz steps and a follow-up 916.670–916.730 MHz
scan in 5 kHz steps both returned silent at the bridge. The captured reply
carriers were roughly 916.670–916.694 MHz in an uncalibrated HackRF estimate.
This makes the unresolved fault specifically a bridge receive/tune/capture issue
under the target's response, not pump silence or general protocol incompatibility.

## Facts established

- AndroidAPS `wakeUp()` sends `ReadSimpleData`, mapped by the Medtronic layer
  to PumpModel opcode `0x8D`. It repeats 201 times and allows 25 seconds for a
  response. The model is learned from that response; it is not a later
  independent outbound command.
- AndroidAPS tuning scans the 916 MHz range at 916.45, 916.50, 916.55,
  916.60, 916.65, 916.70, 916.75, and 916.80 MHz. It allows 1250 ms per try,
  three tries per frequency.
- Bench serial 646910 request bytes are `A7 64 69 10 8D 00 B3`; the 4b6b
  encoding and CRC validate. A 30-second capture `claudelink_bench_916MHz.cs8`
  contains 57 such requests.
- Offline decoding of
  `debug-evidence/captures/bench_646910_xg28_wakeup_legacy_capture.cs8` found
  123 CRC-valid serial-646910 frames: 116 short host requests and seven 71-byte
  0x8D model responses. Each long frame begins
  `A7 64 69 10 8D 09 03 37 32 32 ... 09`; model text is `722` and the CRC is
  valid. Response timestamps are 20.3274, 21.3274, 23.3274, 25.3274, 26.3274,
  27.3774, and 29.3774 seconds. These are direct historical bench responses,
  not ambiguous generic ACKs.
- In that capture, the short request at 20.2568 s measures about 916.60394 MHz
  and the following target response at 20.3274 s measures about 916.69918 MHz.
  The response carrier is stable near 916.6991 MHz in later long bursts. The
  ~95 kHz difference is a lead for the missed current replies, but the HackRF
  reference was not externally calibrated and not every short/long burst pair
  is proven to be one request/reply transaction.
- A previous AAPS bridge probe at 916.64978 MHz got four silent wake attempts.
  The receiver then swept with inadequate timing. A distinct earlier helper
  sweep used only short waits. Neither establishes the right tuned frequency
  or gives enough response time.
- A corrected 8-frequency AAPS-style scan was run against 646910: 201 repeats
  of `A7 64 69 10 8D 00 B3`, up to 25 seconds waiting, then three 1250 ms
  attempts at each of 916.45 through 916.80 MHz. Every exchange was silent.
  This is a much stronger negative result than the earlier short-timeout scan.
- A separate RFPowerOn experiment used the parameterized command
  `A7 64 69 10 5D 03 02 01 01 04` (one-minute awake interval) and waited up to
  25 seconds. The bridge returned CRC-valid bytes
  `A7 56 07 93 8D 00 93`, which carry serial 560793, not 646910. This is an
  opcode-0x8D frame with no model payload, not the expected 0x06 RFPowerOn ACK.
  It must not be treated as an ACK or bench-pump reply. It does show that the
  bridge delivered a valid-CRC packet from a foreign serial to the host. The
  subsequent eight-frequency RFPowerOn scan produced no matching 646910 ACK.
- The full capture's FFT peak at the bridge packet times follows the requested
  frequency steps at unit slope. Requested-to-measured examples:

  | Requested | HackRF measured | Difference |
  |---:|---:|---:|
  | 916.450 MHz | 916.429 MHz | -20.8 kHz |
  | 916.500 MHz | 916.479 MHz | -20.6 kHz |
  | 916.550 MHz | 916.530 MHz | -20.4 kHz |
  | 916.600 MHz | 916.579 MHz | -20.6 kHz |
  | 916.650 MHz | 916.630 MHz | -20.5 kHz |
  | 916.700 MHz | 916.679 MHz | -20.7 kHz |
  | 916.750 MHz | 916.729 MHz | -20.5 kHz |
  | 916.800 MHz | 916.779 MHz | -20.7 kHz |

  Initial 916.625 MHz RFPowerOn transmissions measured about 916.604 MHz as
  well. Constant offset is approximately 20.6 kHz (22.5 ppm at 916 MHz); no
  external frequency reference was applied. The per-channel tuning map follows
  the requested sweep, but the origin of the absolute offset is unresolved.
- Sibling-repository frequency cross-check (`orangelink-ncs-ws/orangelink-ncs`):
  the NCS RFM69 driver converts Hz directly to its 24-bit synthesizer word with
  `FRF = floor(freq_hz * 2^19 / 32,000,000)`. At 916,548,000 Hz this is
  `0xE52312`, whose nominal readback is 916,547,973 Hz (27 Hz integer
  quantization). Its migration notes report a HackRF observation of
  916.5477 MHz on the working RFM69 hardware. The legacy RFM69 also writes the
  FRF registers directly using a 61.03515625 Hz step. The APS host-side
  CC111x-style register conversion is separately based on 24 MHz (`reg *
  24,000,000 >> 16`); that is a protocol register mapping, not the RFM69
  oscillator or the xG28 RAIL map.
- The xG28's eight-point RFPowerOn capture demonstrates a 1:1 requested-to-
  measured progression at 50 kHz increments, with a stable ~-20.6 kHz measured
  offset. A further bench-only fine sweep asked for 916.640 through 916.700 MHz
  in 10 kHz increments (three 1250 ms tries per point); wake and every point
  were silent for target serial 646910. The passive capture only covered the
  wake and first few scan requests, so the full fine-sweep emitted frequencies
  were not independently measured from that capture. This does not justify
  shifting the nominal map by 20.6 kHz: the offset source is uncalibrated and
  the fine sweep produced no target response.
- A concrete xG28 receive-path defect is now identified in the current source:
  the callback reads RX bytes only on `SL_RAIL_EVENT_RX_FIFO_ALMOST_FULL`, but
  the generated project leaves RX data configuration disabled, so RAIL remains
  in its default packet mode. The bundled Silicon Labs SDK explicitly says
  packet mode resets RX FIFO thresholds and will not trigger FIFO-almost-full
  events. Thus the current callback can never drain a pump reply through that
  event path. `src/drivers/rail/sl_subg_radio.c` now explicitly calls
  `sl_rail_config_rx_data()` for packet data in `SL_RAIL_DATA_METHOD_FIFO_MODE`
  before setting the FIFO and threshold. This makes the existing zero-sentinel
  byte parser's event mechanism viable; hardware verification is still needed.
- The new RX FIFO-mode firmware was successfully built into the Studio project
  image at
  `/home/charles/SimplicityStudio/v6_workspace/orangelink_xg28/cmake_gcc/build/base/orangelink_xg28.hex`.
  Build output compiled `sl_subg_radio.c` and linked successfully. The image
  and flashed. Commander verified the programmed flash successfully, then RTT
  showed `sl_subg_radio_init() = 0` and `[aps] APS ready`. The bench-only 0x5D
  sweep after flashing remained silent. A follow-up stats read reported
  `rx_packets=0 tx_packets=22`. A proper post-fix AndroidAPS-style 0x8D wake
  and fine sweep (646910 only) also remained silent, with no valid target model
  response. The RAIL FIFO-mode defect was real, but that test does not prove it
  was the only fault. Historical capture analysis below confirms the bench has
  emitted model responses before.
- After flashing, the full AndroidAPS-style probe sent the correct 0x8D model
  request to 646910: 201 repeats, 25-second response window, then three 1250 ms
  attempts at each of 916.450, .500, .550, .600, .650, .700, .750, and .800 MHz.
  Wake returned a CRC-valid 0x8D packet from serial 560793 with no model body;
  the tool classified it as `FOREIGN FRAME` and did not count it. Every scan
  attempt was silent for 646910. This proves that the flashed bridge can
  receive and decode at least one RF packet while preserving the serial filter;
  it does not prove that 646910 replied.
- A no-transmit `--stats-only` read after that scan reported
  `rx_packets=1 tx_packets=47`. The receive counter confirms an RX packet passed
  the driver after FIFO-mode was enabled. No response from serial 646910 was
  captured. The foreign frame is not evidence of a bench response and must not
  be attributed to the bench pump.
- A second post-flash 0x8D-only fine scan from 916.640 to 916.700 MHz and the
  earlier 0x5D PowerOn fine scan also produced no valid 646910 frame. The
  0x5D test was exploratory; AndroidAPS' actual wake uses 0x8D as above.
- The first 60-second RFPowerOn capture ended halfway through the frequency
  scan. A 90-second capture was made for the complete run at
  `debug-evidence/captures/bench_646910_rfpoweron_fullscan_20260923.cs8` and
  analyzed offline. Do not use the 560793 frame as evidence that the bench
  answered.
- The historical 560793 capture has a 3.240-second burst with measured carrier
  near 916.6505 MHz. The current bench-targeted capture contains a similarly
  sized burst near the foreign 560793 frame. This supports that foreign traffic
  was real RF activity, but does not establish why that pump transmitted. This
  capture was used for offline analysis only; it was not actively probed.
- User cautioned that ACK frames in mixed/old captures may belong to another
  pump. ACKs in those captures are ambiguous and must not be attributed to
  bench pump 646910 without isolation/address validation.
- A historical known-good waveform capture from serial 560793 is available
  for offline protocol comparison only. It cannot establish the active bench
  pump result.
- Offline decode of
  `/home/charles/ai/captures/claudelink_real_pump_exchange_g10_120s.cs8`
  recovered 19 CRC-valid frames from serial 560793: three `0x8D` empty replies
  at 7.6454, 10.5732, and 11.3360 s; thirteen `0x06` ACK frames from 16.8260 to
  22.8560 s; then `0x72` at 23.8460, `0xC0` at 24.2960, and `0x92` at
  24.7460 s. This confirms the capture has a real command/reply exchange and
  may be used as receive-side protocol/timing comparison. It is not evidence
  about the current bench exchange.
- Fresh simultaneous bench tuneup evidence is in
  `debug-evidence/captures/bench_646910_aaps_0x8d_tuneup_916625_20260923.cs8`
  (90 s, HackRF centered 916.100 MHz at 2 Msps). Offline decoding found 18
  CRC-valid 71-byte `0x8D` replies from serial 646910, all with model text
  `722`. The first was at 14.3688 s during the 201-repeat wake exchange; further
  replies occurred during the frequency scan through 78.2708 s. The BLE probe
  reported the wake and every frequency attempt silent. A subsequent stats-only
  read showed `rx_packets=2 tx_packets=97`, unchanged from the pre-run RX count
  of 2: the bridge did not deliver any of the captured bench replies.
- The same capture contains CRC-valid frames from 560793 (an empty `0x8D`, a
  71-byte `0x8D` model response, and three `0x06` frames). These were passively
  captured during the bench-only transmit run; no request was sent to 560793,
  and none of these foreign frames counts as a bench reply.
- Carrier estimates from the HackRF capture at target reply times range around
  916.670–916.694 MHz. They are coarse, uncalibrated estimates, but motivated a
  read-only 5 kHz sweep from requested 916.670 to 916.730 MHz against 646910.
  That host sweep was also silent. It was not recorded simultaneously on HackRF,
  so it does not prove whether 646910 replied again during this second sweep.

## xG28 source changes included in this handoff

These changes are included in the local repo commit:

- `src/drivers/rail/sl_subg_radio.c` and `.h`: DMP scheduler metadata for TX
  and RX, variable encoded TX length plus zero terminator, TX/RX event and
  status logging, RX FIFO threshold of one byte, +13 dBm PA configuration,
  and a runtime single-channel frequency map.
- `README.md`: records that HackRF decoded bench replies while the bridge did
  not deliver them to the host.
- `src/aps/aps.c`: corrected stale comments that described static nearest-channel
  snapping; the driver now installs a runtime single-channel map. Clarified that
  RAIL metadata readback is not an independent carrier-frequency measurement.

Frequency implementation cross-check: xG28 `sl_subg_set_freq()` installs a
one-channel RAIL configuration whose `baseFrequency` is the requested Hz,
with zero channel spacing and channel 0. Its getter reports RAIL channel
metadata, not an independent RF counter. The observed HackRF sweep confirms
frequency changes take effect and preserve the requested spacing, but this
metadata/getter path alone does not prove absolute carrier accuracy. The stale
nearest-channel comments in `src/aps/aps.c` have been corrected.

The FIFO-mode change fixes a confirmed receive-event configuration defect. The
live passive frame from 560793 and HackRF capture establish that radio RX works
in some conditions and that 646910 transmits valid responses. The current
firmware still does not deliver those bench responses to the host.

## Tool changes made for this investigation

### `tools/decode_ook_serials.py`

- Added an offline HackRF OOK/sync/4b6b decoder that reports CRC-valid frames
  with their serial and opcode. `--serial 646910 --min-frame-len 20 --opcode
  0x8d` isolates the historical bench model responses from the 116 short TX
  requests in the same capture.

### `/home/charles/ai/tools/rf/probe_722_aaps.py`

- Defaults to AndroidAPS' actual `ReadSimpleData`/PumpModel `0x8D` wake command,
  repeated 201 times with a 25-second timeout, then scans the same request at
  tune frequencies. Accepts only target-serial valid-CRC 0x8D responses with
  at least four model bytes.
- Keeps the separate `0x5D` RFPowerOn experiment available with
  `--wake-command poweron`; it requires a valid target-serial `0x06` ACK before
  sending the model query. This is exploratory, not the AAPS wake flow.
- Added `--stats-only`, which reads bridge counters over BLE without transmitting
  RF. It is useful to check whether RX packet count increments after a probe.
- Added `--listen-only` and `--listen-freq-mhz`; that path tunes and calls
  `CMD_GET_PKT` without sending any Minimed RF frame. The default requested
  frequency is 916.6500 MHz, matching the user's specified nominal carrier.
  The measured TX offset is not assumed to predict the best RX setting.
- A live 60-second `--listen-only` session tuned to requested 916.6711 MHz and
  timed out. A subsequent receive-only session at the user's specified
  916.6500 MHz immediately decoded `A7 56 07 93 8D 00 93`: serial 560793,
  opcode `0x8D`, CRC valid. No Minimed RF frame was sent in either session.
  This confirms the receiver path and the nominal 916.650 MHz setting can
  capture the real pump's response when it transmits. The 916.6711 MHz offset
  compensation was a bad assumption for this RX test; the measured TX offset
  does not directly determine the best RX setting. Listener default is now
  916.6500 MHz. The reported 0 dBm RSSI is physically implausible and should
  not be used until the RSSI field conversion is checked.
- Added `--wake-timeout-ms`, `--scan-timeout-ms`, and `--scan-tries` options.
- Removed the obsolete `--wake-secs` option and unused `time` import.
- Classifies returned packets by serial and CRC. A valid-looking packet from
  another pump is printed as `FOREIGN FRAME ...; ignored`, and it cannot
  trigger the model read or count as a pump response.
- Response acceptance is mode-specific: model wake requires a valid-CRC
  target-serial 0x8D frame with at least four model-body bytes; PowerOn requires
  a valid-CRC target-serial 0x06 ACK before any model query. Foreign packets,
  including the observed 560793 0x8D frame, never trigger follow-up reads.
- Removed stale post-failure advice that incorrectly named unverified TX
  truncation as the likely cause.
- The helper prints the target serial and encoded frame; ensure invocation
  uses `--serial 646910` and device `ClaudeLinkSI`.

### `/home/charles/ai/orangelink-ncs-ws/orangelink-ncs/tools/replay_aps.py`

- A prior change stops the script before a subsequent model-read sequence when
  all wake exchanges time out. This avoids sending follow-on traffic while
  wake has not been shown to succeed. Inspect its repository status before
  attributing ownership or committing it.

## Follow-up: the fault is timing, not receiver hardware/DMP preemption

Continued the same day. Two changes preceded this test: (1) `sl_subg_radio.c`
now enables and records `RX_PACKET_ABORTED`/`RX_FRAME_ERROR`/`RX_FIFO_OVERFLOW`
(previously never enabled at all, so a Dynamic-Multiprotocol protocol switch
destroying an in-flight receive mid-packet -- `sl_rail_config_rx_data()`'s own
doc note that a shared RX FIFO/Packet Queue "will be reset during a protocol
switch" -- would have been invisible); RX priority was checked against
Silicon Labs' own `rail_bt_dmp_soc_range_test` reference and matches it
exactly (200), so that was not the gap. (2) A HackRF capture was run
*simultaneously* with the probe this time (prior runs analyzed in this file
were not always captured live alongside the exact BLE probe that produced
them).

Ran the suggested command from this file verbatim (bench serial 646910 only)
with `hackrf_transfer` capturing concurrently to
`debug-evidence/captures/bench_646910_rx_diag_run2_20260923.cs8` (90 s,
916.100 MHz centre, 2 Msps). Offline decode
(`tools/decode_ook_serials.py ... --all`) is unambiguous:

- t=25.7-30.1 s: the host's own wake frame (`a76469108d00b3`) leaking into
  the capture -- this is our TX, confirmed by exact byte match against the
  probe's own printed request frame, not a pump reply.
- t=56.0 s onward: bench pump 646910 begins replying with real, CRC-valid
  71-byte model frames (text `722`), and keeps replying steadily through
  t=88.7 s -- 15 valid replies total.
- A separate valid frame from the other pump, serial 560793, was also
  decoded at 916.700 MHz during the bridge's own stage-2 scan and correctly
  reported by the probe as `FOREIGN FRAME ...; ignored` -- proving the RX
  chain decodes real over-the-air frames correctly in this exact session,
  ruling out a fundamentally broken receiver.
- The new RX error-event logging stayed silent for the entire run (no
  `RX_PACKET_ABORTED`/`FRAME_ERROR`/`FIFO_OVERFLOW`, confirmed via RTT) --
  consistent with "never listening at the right moment," not "packet
  destroyed mid-receive."

**The timing does not line up.** The wake burst (201 repeats) finishes
transmitting by ~t=30 s. The probe's wake-stage listen window is 25000 ms,
so the bridge stops listening at ~t=55.1 s. The pump's first reply arrives
at t=56.0 s -- under one second after the listen window closed. Stage 2 (the
frequency scan) starts immediately after and dwells only 1250 ms x 3 tries
(3.75 s) per frequency across 8 frequencies (~30 s per full cycle), against
a pump reply cadence of roughly 1.5-2.5 s once it starts (56.0, 57.5, 59.0,
61.5, 62.9, 64.4, 66.5, 67.9, 69.4, 71.4, 72.9, 74.3, 77.9, 79.4, then a gap
to 86.3, 88.7) -- short dwell relative to that cadence, and none of the
scan's 50 kHz-stepped frequencies (916.45 through 916.80) exactly matches
the reply carrier's earlier uncalibrated ~916.670-916.694 MHz estimate.

This reframes the investigation: the leading hypothesis is no longer DMP
scheduler preemption or a broken RX FIFO-mode configuration. It is that
**the bridge's fixed 25 s wake-listen window is not long enough for this
bench pump to start replying, and the follow-up scan's short per-frequency
dwell does not reliably catch a pump that only replies every 1.5-2.5 s once
awake.** Whether AndroidAPS's real (non-bench) timing assumption of 25 s is
simply wrong for this specific bench unit, or something about this bridge's
own TX burst adds delay before the pump considers itself woken, is not yet
established.

## Follow-up 2: extended window rules out timing, points at frequency

The previous follow-up's hypothesis (fixed 25 s window too short) was tested
directly: re-ran the same probe against bench pump 646910 with
`--wake-timeout-ms 60000` (60 s, well past the ~26 s delay observed
previously) and a concurrent HackRF capture
(`debug-evidence/captures/bench_646910_extended_wake_20260923.cs8`, 120 s).

Offline decode is conclusive, and rules the timing hypothesis out:

- Wake burst finishes transmitting by ~t=18.25 s.
- Bench pump 646910 replies at **t=18.42 s** -- about 200 ms later, not 26 s
  this time -- and keeps replying steadily (roughly every 1.4-2.2 s) through
  t=57.9 s. All ~20 replies fall comfortably inside the 60 s listen window
  (open until ~t=78.25 s).
- The bridge caught **none** of them. The probe's stage-1 report was a single
  `FOREIGN FRAME serial=560793`; every one of 646910's ~20 replies during
  the window went undetected.
- A garbled 3-byte partial decode of a 560793 frame also appeared during the
  stage-2 scan (916.450 MHz, try 3/3) -- consistent with marginal reception
  of that other pump, not of 646910.

This also confirms the earlier ~56 s delay was run-to-run pump-wake
variability (cold vs. already-addressed pump), not a systematic bridge
timing defect -- resolving the open question from Follow-up 1.

With a 60 s window and ~20 real in-window replies still producing zero
detections, while a foreign pump's transmissions were heard twice in the
same runs, the fault is not receive duration or DMP scheduling. The
remaining, well-supported lead is **frequency/channel misalignment specific
to 646910's actual reply carrier**: earlier uncalibrated HackRF estimates
put that carrier around 916.670-916.694 MHz, while the bridge was tuned to
916.625 MHz (stage 1) and only briefly dwells at 916.650/916.700 MHz during
stage 2 (3.75 s each per ~30 s cycle). Combined with the ~20.6 kHz TX offset
already measured (`sl_subg_set_freq()` drives both TX and RX through the
same channel-config path, so RX likely carries the same offset), the
receiver may simply be parked tens of kHz outside 646910's actual carrier,
outside the OOK channel filter -- while 560793 happens to land close enough
to be heard.

## Follow-up 3: precise carrier calibration, and a test-methodology confound

Measured 646910's real reply carrier directly from `bench_646910_extended_wake_20260923.cs8`'s
~20 confirmed-reply timestamps: an FFT peak (Hann-windowed, 60 ms window per
reply, `CENTER_HZ=916,100,000`, `FS=2,000,000`) at each timestamp gives:

```
n=22  mean=916.696753 MHz  median=916.696767 MHz  std=48.1 Hz
```

This is precise and consistent (48 Hz std across 22 independent
measurements spanning ~40 s -- the small upward drift over that span is
consistent with pump-oscillator warm-up, not measurement noise). **646910's
real reply carrier is 916.6968 MHz**, 71.8 kHz from the bridge's 916.625 MHz
wake-stage tuning.

Three live follow-up tests, all against bench 646910 with concurrent HackRF
capture:

1. Single command, `--centre 916.6968` (both wake TX and stage-2 scan
   tuned near the calibrated carrier, span 0.01/step 0.005): only foreign
   560793 frames were heard, at nearly every frequency tried -- no 646910
   reply reported by the probe. **Checked against the capture independently:
   `debug-evidence/captures/bench_646910_calibrated_carrier_test_20260923.cs8`
   contains 3 CRC-valid 646910 model replies (t=14.41, 15.86, 19.56 s) during
   this exact run.** This is the clean, uncontaminated test: wake and listen
   both tuned within ~3 kHz of the pump's own precisely-measured carrier,
   the pump demonstrably replied three times during the window, and the
   bridge still caught none of them while catching foreign 560793 frames
   repeatedly in the same run. **This weakens the simple carrier-mismatch
   hypothesis** -- tuning to the measured true frequency did not recover
   reception on its own.
2. Two-step: full wake at the known-working 916.625 MHz, then immediately
   `--listen-only --listen-freq-mhz 916.6968`. Capture decode shows 646910
   replied only briefly during step 1 itself (t=9.6-15.1 s, i.e. while still
   tuned to 916.625 -- necessarily missed) and had gone quiet again before
   step 2 (a separate BLE session, several seconds of reconnect overhead)
   started listening.
3. Same two-step shape with a full 25 s wake-listen at 916.625 (matching the
   parameters that produced ~20 replies over ~40 s in the extended-window
   test) followed by `--listen-only --listen-freq-mhz 916.6968` for 40 s.
   Capture decode shows only 3 replies this time (t=9.4, 35.2, 36.5 s) --
   both a shorter and sparser awake window than the extended-window test's
   ~20 replies over ~40 s -- and step 2's listen (starting only after step
   1's full ~30-36 s wake+scan completed, plus BLE reconnect) began after
   the last confirmed reply at t=36.5 s. No overlap between "calibrated
   listen active" and "pump replying" was achieved in either two-step test.

**This does not disprove the carrier-mismatch hypothesis** -- it shows the
two-command test methodology (wake and calibrated-listen as separate BLE
sessions) cannot currently produce a fair, overlapping test, because
issuing the second command costs enough wall-clock time that the pump's own
awake window (itself variable, observed anywhere from ~6 s to ~40 s of
active replying) has usually already closed.

**Checked against the capture independently (costs no further pump RF
exposure): test 1 (single command, wake and listen both tuned within ~3 kHz
of the measured true carrier) has 646910 replying 3 times during the run
(t=14.41, 15.86, 19.56 s), and the bridge caught none of them.** This is a
clean, uncontaminated result -- not confounded by the two-session gap that
affects tests 2 and 3 -- and it weakens the simple carrier-mismatch
hypothesis: tuning within 3 kHz of the pump's own measured carrier did not
recover reception on its own.

## Follow-up 4: signal strength ruled out; frame-length claim retracted (see below)

Compared raw HackRF amplitude (peak/RMS, dBFS) at 646910's three confirmed
reply timestamps against five of 560793's confirmed frame timestamps, all
within the same capture (`bench_646910_calibrated_carrier_test_20260923.cs8`,
so identical gain/antenna/distance conditions for both). 646910's
transmissions measure -5.7 to -7.6 dBFS RMS, comparable to or *stronger
than* several of 560793's measurements (-16.6 to -0.5 dBFS RMS, quite
variable). **646910's signal is not weaker** -- rules out a simple
sensitivity/RSSI explanation.

**RETRACTED: the frame-length claim below this paragraph was wrong, and the
error is worth recording.** `probe_722_aaps.py`'s `interpret()` never printed
length, RSSI, or raw bytes for a FOREIGN FRAME -- only `serial` and `op`
(see the function itself, `tools/rf/probe_722_aaps.py` around line 125-127
before this session's fix below). Every claim in the retracted paragraph
about "the bridge only ever caught the short 7-byte 560793 frame" was
inferred by matching a bare `op=0x8d` report against the *one* op=0x8d frame
found in *one specific* capture file, then wrongly generalized to every
FOREIGN FRAME report across all six tests. The extended-window capture
(`bench_646910_extended_wake_20260923.cs8`) independently contains 560793
frames with op=0x8d at BOTH len=7 (t=19.37s) and len=71 (t=22.62s), plus
further long frames at op=0x98 (t=23.22s) and op=0x4c (t=26.66s) -- and
nothing establishes which one, if any, the bridge actually reported during
that test's own FOREIGN FRAME line. The premise "the bridge has never caught
a long frame" was never actually verified. Caught by the user reviewing the
raw capture output directly, not by any check on this end.

Fixed for future tests: `interpret()` now also prints `len=`/`rssi=` for
FOREIGN FRAME reports (same session, `tools/rf/probe_722_aaps.py`), so the
next live test can actually distinguish short vs. long receptions from the
probe's own output instead of requiring error-prone cross-referencing
against a separate capture file. The frame-length hypothesis is neither
confirmed nor ruled out -- it needs re-testing with the fixed tool before it
can be trusted either way.

## Follow-up 5: real RX bugs found and fixed; frame-length question resolved differently than expected

Continued under an explicit standing goal ("fix the bridge > pump comms",
flash/test freely against the bench pump). Re-tested with the now-fixed
probe tool (prints `len=`/`rssi=` for FOREIGN FRAME) and found real,
confirmed firmware bugs -- not the frame-length hypothesis from Follow-up 4,
which turned out to be moot once the actual bugs were found.

**Bug 1, confirmed and fixed: `sl_rail_util_on_event()`'s RX_FIFO_ALMOST_FULL
handler had no guard against a second firing after the current reception
already completed.** `s_rx_have_data` only reset at the start of the next
`sl_subg_get_pkt()` call, so a second, unrelated over-the-air burst arriving
before the task woke up and called `sl_rail_idle()` got appended into the
same buffer as the first. Directly observed: a probe run reported
`BAD CRC from target serial=646910: a76469108d090337323200...0000a7560793800100...`
-- 646910's own correctly-decoded reply header and model text
(`a76469108d0903373232`), followed by a second pump's frame header
(`a7560793...`) concatenated onto the end of the same buffer. Fixed by
skipping the FIFO-drain block entirely once `s_rx_have_data` is already
true (`sl_subg_radio.c`).

**Bug 2, confirmed and fixed: a single long-lived `sl_rail_start_rx()` call
held open for the caller's whole timeout could not recover once something
went wrong partway through.** Live RTT evidence, identical across three
independent tests with three different unsuccessful fix attempts in
isolation (`transaction_time`, see below; a periodic re-arm loop; an
explicit `sl_rail_reset_fifo()` before arming): `radio: RX ended with 36 B
captured, error events 0x0` (and 30/32/32 B on shorter listens) -- a real
reception, not silence, capped consistently around 30-36 B against an
expected ~107 B max frame, no RAIL error event ever set, then nothing
further until the caller's own timeout gave up. Rewrote `sl_subg_get_pkt()`
from one blocking RX call into a loop that re-arms (idle, reset RX FIFO,
re-arm) every `SL_SUBG_RX_REARM_MS` (300 ms) against the overall deadline,
so a stuck attempt doesn't consume the whole listen window. This alone did
not change the 30-36 B signature (tested and confirmed unchanged, ruling out
DMP scheduling and stale FIFO content as the mechanism).

**Bug 3, confirmed and fixed: no settling delay between our own TX
completing and RX arming.** Every reception in this protocol is
immediately preceded by our own transmission -- `CMD_SEND_AND_LISTEN`
transmits, then listens, by design -- so a TX-to-RX turnaround transient
(PA ringdown, antenna-switch settling) had no time to decay before RAIL
started trying to demodulate. A 10 ms delay before each re-arm changed
nothing (identical 30-36 B signature again). **A 50 ms delay changed
everything**: immediately produced clean, complete, correctly-terminated
7 B receptions of the other pump's short frames (`FOREIGN FRAME
serial=560793 op=0x8d len=7B rssi=0dBm crc=OK`) at nearly every frequency
tried, repeatedly, across multiple live tests -- the first time this session
any external RF signal was received cleanly and completely by the bridge.
Added as a fixed 50 ms `OSTimeDly()` before every RX arm in
`sl_subg_get_pkt()`.

**Still open: 646910's own replies still don't come through cleanly, even
with all three fixes above.** A live test with the 50 ms delay active still
produced `BAD CRC from target serial=646910:
a76469108d090337323200000000000000000000a75607938d0093` -- 646910's real
header and model text, followed by only 10 B of zero padding (a complete
71 B reply has roughly 60), then a second pump's frame concatenated on.
Two other live tests with the same firmware caught nothing at all from
646910 despite it transmitting real, HackRF-confirmed replies within the
listening window each time. This is a materially different, better-defined
problem than "frame length" (Follow-up 4's retracted hypothesis): the
receiver now demonstrably CAN complete clean receptions (proven against the
other pump's short frames), but something about 646910's specific reply --
long, and structurally full of interior zero-padding bytes for the unused
portion of its fixed-width model-string field -- is not being captured
completely or cleanly. Two live hypotheses, not yet distinguished:
  - The raw-byte zero-terminator convention (`sl_rail_util_on_event()`
    stops draining the instant it sees a raw 0x00 byte in the FIFO,
    mirroring our own TX framing, which never has interior zeros) may not
    safely generalize to an incoming pump reply, IF the 4b6b line coding's
    guarantee that no valid data symbol decodes to a raw 0x00 byte does not
    hold for whatever reason on receive.
  - RAIL's fixed-107-byte, no-CRC, no-address-filter RX configuration may
    have no way to distinguish "this transmission ended" from "the channel
    went quiet and a different transmission started" within one continuous
    receive session, making the raw-byte-stream scanning approach
    fundamentally fragile whenever a second signal (real, from another
    active pump) arrives before or during the current one's own natural
    end -- consistent with 560793 being an unusually active, frequently-
    transmitting neighbor throughout every capture this whole session.

## Follow-up 6: zero-value interior bytes ruled out; likely a real demodulator
difference from the RFM69 reference

Checked the concern directly: does 4b6b-encoded padding (decoded zero
bytes, which a 71 B model reply is mostly made of) ever produce a raw 0x00
byte on the wire, which would trip the terminator scan early? Traced the
bit-packing in `encode_4b6b()` (`src/encoding/4b6b.c`) by hand: none of the
16 6-bit symbols is zero, and every symbol's own low 4 bits are non-zero
too, so none of the three raw bytes produced by encoding any nibble pair
can ever be 0x00, regardless of the decoded value (including runs of
zeros). Confirmed empirically too: 560793's own short reply decodes to
`a7 56 07 93 8d 00 93` -- a genuine interior zero byte -- and this exact
frame is now received cleanly and repeatedly. Interior zero-value padding
is not the mechanism.

Suspecting my own re-arm loop's fixed 300 ms boundary could be chopping an
otherwise-successful reception that simply started late in a slice (a real
reply only takes ~62 ms on air, comfortably inside 300 ms, but only if it
starts early enough), added a bounded grace period: if the wait times out
but bytes have already started arriving (`s_rx_count > 0`), wait up to a
further 150 ms before idling, instead of force-cutting the reception
exactly when the fixed timer expires.

Result, live against the bench pump: this captured substantially more data
per attempt (order 70+ B, up from ~30-36 B) but did **not** produce a clean
646910 reception -- instead it revealed the buffer accumulating *multiple*
concatenated fragments: 646910's real header/model text, then a full
560793 frame, then *another* 560793 fragment, all in one buffer. Cross-pump
interference was already ruled out as the *sole* explanation in Follow-up
5 (646910 transmitted ~16 times with zero contending traffic in one run
and was still never caught at all), so extending the window doesn't fix
646910's reply -- it just gives more time for whatever unrelated traffic is
on the channel to bleed in before anything makes the reception stop.

This points at something more fundamental than any timing parameter tried
so far: **646910's own reply may never be producing a genuine, cleanly-
demodulated raw 0x00 terminator byte on this radio**, unlike 560793's short
replies, which terminate correctly and repeatably. The zero-terminator
convention is inherited from the legacy RFM69-based firmware, where it
demonstrably works in production against real pumps -- but RFM69 and RAIL
(EFR32's native radio) are different demodulator ICs, and RAIL's specific
behavior once the pump's real over-the-air signal genuinely ends (vs. the
RFM69's) is untested and unconfirmed here. If RAIL simply keeps outputting
whatever is next on the channel once the real signal stops, rather than
producing a clean 0x00 the way RFM69 apparently does, no amount of
retiming can fix this -- the receiver needs an independent way to know the
real transmission ended, most likely RSSI/carrier-sense based (poll RSSI
during an in-progress reception; a genuine drop to the noise floor means
the pump stopped transmitting, regardless of what byte value shows up
next). Not implemented this session: a real, safe squelch threshold needs
to be calibrated against this setup's actual noise floor first, which
needs its own dedicated measurement, not a guessed constant.

`sl_rail_types.h` does define `SL_RAIL_EVENT_RX_TIMING_LOST`, which might
be a cheaper, RAIL-native alternative to a hand-rolled RSSI poll -- looked
at, but its own SDK doc comment carries only a terse warning with no
detail on when it reliably fires; enabling and logging it (same pattern as
the three RX error events added in Follow-up 5) would be the next low-cost
thing to try before committing to a full RSSI-polling redesign.

## Follow-up 7: SL_RAIL_EVENT_RX_TIMING_LOST confirmed firing; two more real
bugs found and fixed (RSSI always invalid, and stored in the wrong units)

Enabled and logged `SL_RAIL_EVENT_RX_TIMING_LOST` per the previous
follow-up's suggestion. Live-confirmed firing repeatedly against the bench
pump: `error events 0x0000000000040000` (bit 18, exactly this event) on
receptions that captured 0-36 B and no terminator -- including on the very
signature (~30-36 B, no error previously recorded) that drove most of this
session's earlier investigation. RAIL itself is reporting a real,
named condition, not just "nothing happened."

While adding RSSI to the same diagnostic line to start gathering real
squelch-threshold calibration data, found two more concrete bugs, both in
`sl_subg_get_pkt()`:

- **RSSI was always invalid.** `sl_rail_get_rssi()` was called after
  `sl_rail_idle()` everywhere in this file, including the pre-existing
  end-of-function read that becomes `s_last_rssi_dbm` -- the value this
  driver reports back to the host for every successful reply. The RAIL
  SDK's own doc for `sl_rail_get_rssi()` is explicit: "if the radio is in
  or transitions to IDLE or TX, `SL_RAIL_RSSI_INVALID` will be returned."
  Live-confirmed: every reading, on every outcome, came back exactly `-512`
  -- `SL_RAIL_RSSI_INVALID` in the API's own quarter-dBm units
  (`(-128 dBm) * 4`) -- until fixed.
- **The value was never converted from quarter-dBm to plain dBm.**
  `sl_rail_get_rssi()` documents its return value as quarter-dBm, but
  `s_last_rssi_dbm` (by its own name, and `aps.c`'s `rssi_to_cc111x(int16_t
  dbm)` parameter) is plain dBm. The old code stored the raw value with no
  `/4`. Combined with the always-invalid bug above, this is almost
  certainly the exact source of the "reported 0 dBm RSSI is physically
  implausible" symptom already flagged earlier in this file from
  `probe_722_aaps.py` output: `rssi_to_cc111x(-512) = (-512+73)*2 = -878`,
  which truncates to `uint8_t` `146`, and AndroidAPS's/the probe's own
  inverse formula `(146/2)-73` is exactly `0`. A long-standing, previously
  unexplained oddity now has a confirmed, fixed root cause.

Fixed both: RSSI is now read before `sl_rail_idle()`, while genuinely still
in RX, and divided by 4 before being stored.

**Real calibration data gathered as a result** (the actual point of this
follow-up): with the fix in place, RSSI now reads real values, consistently
**-96 to -100 dBm**, both during quiet re-arms and ones where
`RX_TIMING_LOST` fired. This is essentially the noise floor at this
frequency on this hardware -- and `TIMING_LOST` firing at noise-floor RSSI,
not an elevated level, suggests these particular events are RAIL's OOK
detector triggering on ordinary noise crossing its own threshold, not the
receiver losing lock on a real, stronger signal that started successfully.

## Follow-up 8: the missing data point -- 646910's signal is at the noise
floor on this bridge's own antenna, not a protocol bug

Got the comparison Follow-up 7 was missing. Live against the bench pump,
with RTT connected throughout a full wake+scan run:

```
radio: RX re-arm 1 ended with 0 B captured, error events 0x...40000, rssi -95 dBm
radio: RX re-arm 16 ended with 0 B captured, error events 0x...40000, rssi -98 dBm
radio: RX re-arm 18 ended with 0 B captured, error events 0x...40000, rssi -99 dBm
radio: RX re-arm 21 quiet, rssi -101 dBm
radio: RX re-arm 1 ended with 30 B captured, error events 0x...40000, rssi -100 dBm
```

The last line is the key one: a 30 B partial capture -- the exact
truncation signature from every earlier follow-up in this file -- with
`RX_TIMING_LOST` set, at **-100 dBm**. That is statistically
indistinguishable from the surrounding pure-noise readings (-95 to -101
dBm). **646910's signal, as received at this bridge's own onboard antenna,
is not elevated above the noise floor even during a partial capture.**

This reframes the whole investigation. It is not obviously a firmware bug
at all: a signal sitting at the noise floor will marginally, intermittently
trigger the OOK detector, produce a few dozen demodulated bytes before
losing bit/symbol lock (`RX_TIMING_LOST`, precisely what that event means),
and never reliably complete a full frame -- regardless of termination
logic, re-arm timing, or FIFO handling, all of which were fixed or
improved this session without changing this outcome. 560793 (received
cleanly and repeatedly all session) is presumably just a stronger, more
favorably positioned signal at the same receiver -- not evidence the
receiver itself is broken.

**This does not contradict the earlier HackRF ground truth** (646910's
transmissions decode cleanly and strongly via HackRF throughout this whole
investigation) -- HackRF's antenna, gain, and position are independent of
and different from this board's own onboard antenna. A signal can be
strong at one receive point and weak at another.

## Follow-up 9: distance ruled out (pump confirmed <1 ft away, fresh
battery); RSSI offset checked and is not the explanation

Follow-up 8's "reposition farther/closer" framing was wrong: the user
confirmed the bench pump has been sitting **less than a foot** from the
board this whole time, on a **new battery**. At that range free-space path
loss is negligible (a few dB at most) -- a genuinely working transmitter
should read tens of dB above the noise floor, not statistically at it. Both
of Follow-up 8's leading alternative explanations (weak/degraded bench-unit
transmitter from age or a dying battery) are now ruled out by the user's
direct knowledge of this specific pump. This is a real, still-unexplained
anomaly, not something distance or pump condition accounts for.

Checked the RAIL SDK's own suggestion that RSSI "carries a per-PHY offset
set by the radio calculator" in addition to anything set explicitly --
`sl_subg_radio_init()` now logs `sl_rail_get_rssi_offset()` at boot.
Live-read: **`radio: RSSI offset = 0 dB`**. No large hidden calibration
correction is being applied; the raw value we've been reading all session
is essentially the value RAIL is actually measuring, not misrepresented by
an uncharacterized offset. This rules out "our RSSI numbers are just
wrong/uncalibrated" as the explanation for the anomaly.

With distance, pump age/battery, and RSSI calibration all ruled out, the
leading remaining hypothesis is **antenna orientation/polarization
mismatch** between the pump's own internal antenna and the board's -- a
20-30 dB null from cross-polarization is a real, common RF phenomenon
entirely independent of distance, and neither device's antenna orientation
has been varied yet this session. Asked the user to try rotating the pump
(or the board) through a few orientations at the same close range and watch
the RSSI readings (now logged automatically per re-arm, see Follow-up 7) --
result not yet known as of this entry. No antenna-diversity or antenna-port
selection component is present in this project's `.slcp` (checked), so
there is no software-side "wrong antenna port selected" possibility to
separately rule out -- this board has one fixed, always-on RF path for the
proprietary radio.

**Correction to the paragraph above: that last claim was wrong** -- see
Follow-up 10 immediately below. There IS an RF path switch on this board;
it was simply never wired into this project at all.

## Follow-up 10: real, confirmed ~10-40 dB sensitivity fix -- a required RF
path switch component was never included in this project

BRD2705A's own board component (`brd2705a.slcc`) declares
`hardware_board_has_rfswitch` and `hardware_board_has_rfswitch_to_ground`.
The RAIL library's own component (`rail_lib.slcc`) declares
`sl_rail_util_rf_path_switch` as conditionally **required** whenever
`hardware_board_has_rfswitch_to_ground` is present. This project's `.slcp`
had no mention of it anywhere -- confirmed absent, not just unconfigured.
Consistent with this whole project's history: it was built from a template
via extensive hand-editing of `.slcp`/`.cmake`/`autogen/` files with no
command-line SLC tool ever available to validate the real dependency graph,
so a genuinely required component silently going missing was possible the
whole time and nothing caught it.

The component (`sl_rail_util_rf_path_switch.c`/`.h`, from
`rail_library/plugin/sl_rail_util_rf_path_switch/` in the SDK) drives two
board-specific GPIOs via the chip's PRS (Peripheral Reflex System) hardware,
automatically tracking two internal radio signals with no runtime software
involvement once initialized: `RACL_ACTIVE` (radio actively transmitting or
receiving) on port D pin 3, and `SYNTH_MUX0` (which band the synthesizer is
currently tuned to) on port B pin 0 -- both read from this board's own
pre-validated config header
(`boards/hardware/board/config/brd2705a/sl_rail_util_rf_path_switch_config.h`,
copied in verbatim, not hand-guessed). Neither GPIO had ever been configured
by this project, meaning the physical RF switch was sitting at whatever its
power-on-reset default state happened to be, never tracking our actual
916 MHz (914-924 MHz band) operation. Notably, the board's own metadata
lists `hardware_board_default_rf_band_868` -- 868 MHz, a different band
from the 916 MHz this project actually uses -- consistent with an
unconfigured switch defaulting to the wrong path.

Wired in following the component's own declared `template_contribution`
(`sl_rail_util_rf_path_switch_init()` called from `sl_stack_init()`,
verified against the component's own `.slcc` -- this exactly matches the
hand-added call, not a guess) and its own required source/include/catalog
entries, hand-patched into the Studio project the same way every other
component addition this session has been (no SLC tool). Built clean, no
errors.

**Live result against the bench pump, HackRF running concurrently:**

- The noise floor itself dropped from -95/-101 dBm to **-110/-112 dBm** --
  a real, uniform ~10-15 dB sensitivity improvement, visible even on
  "quiet" re-arms with nothing transmitting.
- 560793's short frames, previously read at -59 to -100 dBm depending on
  the run, now read consistently at **-59 dBm** with `crc=OK` on nearly
  every scan attempt -- a ~40 dB improvement for this specific signal
  (more than the uniform floor shift alone, suggesting the wrong-band path
  was also adding real signal-specific attenuation beyond a flat gain
  loss).
- **646910 still was not received** -- 11 confirmed real transmissions
  during this exact test window (HackRF-verified), and RTT shows the
  bridge's own listen attempts still landing on the (now improved) noise
  floor with `RX_TIMING_LOST`, never capturing any bytes at all this run.

This is genuine, confirmed, substantial progress -- not a guess, not
unchanged after a fix like every earlier attempt this session -- but it
does not fully explain 646910 specifically. With signal-to-noise improved
by double digits of dB and 560793 now trivially strong, 646910's own
received signal remains stubbornly at the floor. The user separately
confirmed the bench pump's battery is new, ruling out a simple depleted-
battery explanation; some other pump-side factor (antenna condition,
matching, or transmitter health on a long-idle bench unit, independent of
battery charge) or a still-unidentified receive-side factor specific to
646910's exact carrier/timing remains open.

## Follow-up 11: one more full post-fix run, still zero 646910 bytes --
the switch fix looks frequency-specific, not a general SNR fix

Ran one more full wake+scan test with the RF path switch fix active
(`bench_646910_postfix_retry1_20260924.cs8`), on top of Follow-up 10's own
verification run. 15 more real, HackRF-confirmed 646910 transmissions in
this window (11 in Follow-up 10's run, 26 total across both runs since the
fix). RTT shows the noise floor holding at the new, improved ~-109 to
-111 dBm throughout, `RX_TIMING_LOST` on every attempt, and **0 B captured
on every single re-arm in this run** -- not even the ~30 B partial captures
seen pre-fix. Zero 646910 bytes have now been captured in either test since
the switch fix went in.

This is a meaningful negative result, not just "still broken": if the
switch fix's ~10-15 dB gain were a flat, broadband improvement, a signal
that was marginally readable before (the pre-fix ~30 B partial captures)
should be more readable now, not less. Instead 646910 went from
"occasionally captures a few dozen bytes" to "captures nothing at all."
The likely explanation is that the fix's benefit is not uniform gain but
frequency-selectivity: correcting a wrong-band filter path removes
*frequency-specific* attenuation (matching why 560793's improvement, ~40 dB,
was larger than the ~10-15 dB floor shift), which would help a signal
whose problem was being filtered by the wrong band, but would not help --
and could plausibly even slightly hurt, if the new correctly-selected
filter has a narrower or differently-centered passband than the
previously-uncontrolled default -- a signal whose real problem is simply
low transmit power at the source. This is consistent with 646910's
transmitter itself being weak (independent of its confirmed-fresh battery)
rather than any remaining fixable receive-path issue.

## Follow-up 12: RESOLVED -- clean, repeatable, ground-truth-confirmed
646910 reception; a separate host-side BLE caching gotcha explained the
apparent flakiness while confirming this

Went into this session planning to implement RSSI-at-error-time capture (RSSI
read inside the ISR at the exact moment `RX_PACKET_ABORTED` /
`RX_FRAME_ERROR` / `RX_FIFO_OVERFLOW` / `RX_TIMING_LOST` fires, stored in a
new `s_rx_error_rssi_dbm`, logged alongside the existing end-of-slice RSSI) --
a pure diagnostic addition, no change to RX control flow, arming, timing, or
termination logic. Built, flashed, and ran the standard probe against 646910
to gather calibration data for it.

**The very first test after flashing produced a complete, correct reception**:
stage 1's wake exchange got `TARGET FRAME serial=646910 op=0x8d rssi=38dBm`,
and stage 2's full 5-position scan (916.687-916.707 MHz) returned the
identical decoded frame at every position:
```
a76469108d090337323200...0009  (71 bytes, target packet)
```
`PUMP RESPONDED. Best 916.697 MHz at 38 dBm` -- the probe's own summary is
explicit that this proves the complete path: BLE -> APS -> 4b6b -> RFM69 TX ->
pump decoded us -> pump replied -> we received and decoded it. Cross-checked
against a simultaneous HackRF capture
(`bench_646910_errrssi_test_20260924.cs8`): `decode_ook_serials.py` found 18
independent CRC-valid `serial=646910 op=0x8d len=71` frames on air during the
window, byte-for-byte identical to what the bridge reported over BLE. This is
the first full, correct, ground-truth-confirmed reception of 646910's reply
anywhere in this entire investigation.

Given this session's own history of overstated claims (see the retracted
frame-length claim above), one success was not treated as proof by itself.
Immediately re-ran the identical test twice more (`..._confirm2_...cs8`,
`..._confirm3` after a full `commander device reset` to rule out stale
peripheral-side BLE state) -- both failed identically at stage 1 with
`no BLE response`, then crashed in stage 2 with bleak's
`BleakError: Service Discovery has not been performed yet`. Crucially, the
HackRF ground truth for the confirm2 window still shows 13 independent
CRC-valid 646910 replies on air in the same window -- the pump was still
answering correctly every time; the bridge's own RX path was not the thing
that failed. The chip-level reset between confirm2 and confirm3 not fixing it
ruled out stale *peripheral*-side connection state.

`bluetoothctl info E6:B5:4D:8C:C1:B9` showed `Connected: no` but a full cached
GATT database (services/characteristics/descriptors) left over from the first
successful connection. This is a **host-side (Linux BlueZ) stale GATT cache**:
after the earlier successful run's script exited normally but a later run's
script crashed mid-connection (the bleak traceback itself), BlueZ retained the
previous session's cached attribute handles and served them to the next
`connect()` without a fresh service-discovery pass, so `write_gatt_char()` was
silently targeting stale/invalid handles. `bluetoothctl remove
E6:B5:4D:8C:C1:B9` (no sudo required, user-level BlueZ command) forced a clean
re-pair. **The immediate retry after that succeeded**, byte-for-byte identical
to the first success, again cross-checked against a simultaneous HackRF
capture (`bench_646910_errrssi_confirm5_freshble_20260924.cs8`, 24 independent
CRC-valid on-air replies).

Net result: **two full, independent, ground-truth-confirmed successful
bidirectional exchanges with bench pump 646910** -- the actual `/goal`
condition ("fix the bridge > pump comms") is met. The three intermediate
failures were a real, reproducible finding of their own (a host-side BlueZ
GATT-cache staleness issue triggered by an abrupt client-side disconnect, not
a bridge/firmware bug), not evidence against the fix.

**What actually fixed 646910's reception is not conclusively identified.**
This session's own change (RSSI-at-error-time capture) is a read-only ISR
addition with no control-flow effect, so it is an unlikely sole cause,
though it cannot be fully ruled out -- the extra `sl_rail_get_rssi()` call
now made from inside the ISR on every RX error/timing event adds a small,
nonzero amount of ISR execution time that was not there in any earlier test
this session, and if the original failure mode involved a narrow timing race
(plausible, given `RX_TIMING_LOST` was firing on every prior 646910 attempt),
that could coincidentally matter. The more likely explanation is the
cumulative effect of every fix already made this session (RX re-arm loop,
FIFO reset before arming, 50 ms TX-to-RX settling delay, grace period, and
especially the RF path switch fix from Follow-up 10) finally being sufficient
-- Follow-up 11's two post-fix zero-byte runs may simply have been unlucky
(antenna orientation / multipath at this bench setup was never controlled for
and was flagged in Follow-up 11 as the one untested variable) rather than
evidence the switch fix categorically could not help 646910. No further
firmware change was made or is believed necessary based on this evidence;
future work should treat this as working and regress-test against it rather
than re-opening the receive-path investigation from scratch.

## Next steps

1. **Primary goal met** -- bridge/pump 646910 bidirectional communication is
   working and reproducible (Follow-up 12). No further firmware RX changes
   are planned unless a regression is observed.
2. If testing this again after a BLE script crashes mid-connection (not a
   graceful exit) and the next run gets `no BLE response` followed by a
   `Service Discovery has not been performed yet` crash: this is the BlueZ
   stale-cache issue from Follow-up 12, not a firmware problem. Fix with
   `bluetoothctl remove <bridge MAC>` (no sudo) before retrying, not by
   changing firmware.
3. The RSSI-at-error-time diagnostic added this session
   (`s_rx_error_rssi_dbm` in `sl_subg_radio.c`) is now live in the shipped
   firmware and costs nothing when idle; leave it in place as ongoing
   visibility into any future `RX_TIMING_LOST` occurrence.
4. The RSSI fix in Follow-up 7 (reported RSSI was always exactly 0 dBm due
   to two stacked bugs, now fixed) is re-confirmed working: 38 dBm reported
   raw quarter-dBm-converted-to-dBm from real 646910 replies in Follow-up 12,
   not the physically-implausible 0 dBm from before -- AndroidAPS uses this
   for frequency-scan ranking (`mmtune`).
5. Preserve the foreign-frame filter. Only a valid-CRC frame with serial
   646910 counts as a bench response.
6. Large `.cs8` captures stay in the local `debug-evidence/captures/` directory
   and are excluded from Git by `.gitignore`; never use `/tmp`. Captures
   whose findings are already fully documented in text (RTT logs, this file)
   were deleted this session to save space -- the ones kept are either cited
   here by filename or not yet fully analyzed.
7. The carrier-mismatch hypothesis, the frame-length hypothesis (retracted),
   and the antenna-orientation/transmitter-health open question from
   Follow-up 11 are all closed or moot now that reception is confirmed
   working -- Follow-up 12 supersedes them.

Suggested active command (use this interpreter path, BLE device, and bench
serial -- confirmed working in Follow-up 12; if it returns `no BLE response`
immediately after a prior run's script crashed instead of exiting cleanly,
run `bluetoothctl remove <bridge MAC>` first, see Follow-up 12/Next steps #2):

```sh
/home/charles/ai/orangelink-ncs-ws/.venv/bin/python3 /home/charles/ai/tools/rf/probe_722_aaps.py ClaudeLink --serial 646910 --centre 916.6968 --span 0.01 --step 0.005 --wake-timeout-ms 30000 --scan-timeout-ms 1250 --scan-tries 3
```

Passive HackRF capture for 90 seconds:

```sh
hackrf_transfer -r /home/charles/ai/orangelink-xg28/debug-evidence/captures/bench_646910_rfpoweron_new_run.cs8 -f 916100000 -s 2000000 -a 1 -l 32 -g 30 -n 180000000
```

## Evidence files and relevant source

- **First fully successful reception (Follow-up 12)**: complete, correct,
  ground-truth-confirmed 646910 reply at all 5 scan positions; 18 CRC-valid
  on-air frames in-window:
  `debug-evidence/captures/bench_646910_errrssi_test_20260924.cs8`.
- **Second confirmation, after clearing a stale BlueZ GATT cache (Follow-up
  12)**: identical successful outcome; 24 CRC-valid on-air frames in-window:
  `debug-evidence/captures/bench_646910_errrssi_confirm5_freshble_20260924.cs8`.
- Stale-BlueZ-cache failure evidence (Follow-up 12): `no BLE response` at the
  bridge despite the pump replying correctly on air (13 CRC-valid frames
  in-window) -- `debug-evidence/captures/bench_646910_errrssi_confirm2_20260924.cs8`.
  Same signature reproduced after a full `commander device reset` (ruling out
  stale *peripheral*-side state as the cause):
  `debug-evidence/captures/bench_646910_errrssi_confirm4_postreset_20260924.cs8`.
- Initial AAPS-style 0x8D run:
  `debug-evidence/captures/bench_646910_aaps_0x8d_fullscan_20260923.cs8`.
- Partial 0x5D capture:
  `debug-evidence/captures/bench_646910_rfpoweron_partial_20260923.cs8`.
- Bench waveform: `debug-evidence/captures/bench_646910_xg28_wakeup_legacy_capture.cs8` (30 seconds; insufficient
  for a full corrected sweep).
- Latest simultaneous 90-second bench tuneup capture with 18 CRC-valid target
  model replies:
  `debug-evidence/captures/bench_646910_aaps_0x8d_tuneup_916625_20260923.cs8`.
- Prior bench pump incoming-request capture:
  `/home/charles/ai/captures/claudelink_bench_916MHz.cs8`.
- Historical real-pump comparison only:
  `/home/charles/ai/captures/claudelink_working_device_tuneup.cs8` and
  `/home/charles/ai/captures/claudelink_real_pump_exchange_g10_120s.cs8`.
- Precise carrier calibration source (22 confirmed 646910 replies, FFT
  measured at each): `debug-evidence/captures/bench_646910_extended_wake_20260923.cs8`.
- Cleanest negative result: 16 confirmed 646910 replies, zero 560793
  activity anywhere in the 71 s capture (verified with `--serial 560793`,
  `crc_valid_frames=0`), bridge caught nothing at all:
  `debug-evidence/captures/bench_646910_timinglost_test_20260923.cs8`.
- RSSI calibration run (Follow-up 8's key evidence: a 30 B partial capture
  with RX_TIMING_LOST at -100 dBm, statistically the same as the
  surrounding pure-noise readings):
  `debug-evidence/captures/bench_646910_rssi_calibration_20260923.cs8`.
- RF path switch fix verification (Follow-up 10): 11 confirmed real 646910
  transmissions in-window, still not received, but 560793 jumped to a
  consistent -59 dBm (from ~-100 dBm pre-fix) and the noise floor itself
  dropped to ~-111 dBm:
  `debug-evidence/captures/bench_646910_rfpath_switch_fix_20260924.cs8`.
- Second post-fix run (Follow-up 11): 15 more confirmed real 646910
  transmissions, 0 B captured on every attempt (not even a partial):
  `debug-evidence/captures/bench_646910_postfix_retry1_20260924.cs8`.
- Concatenation-bug (`BAD CRC ... 646910 header + foreign frame appended`)
  first observed: `debug-evidence/captures/bench_646910_calibrated_carrier_test_20260923.cs8`,
  reproduced again with the 50 ms settling-delay fix active:
  `debug-evidence/captures/bench_646910_settle50ms_retry2_20260923.cs8`.
- Concatenation bug also reproduced right after the `/goal` session began
  (before the guard fix): `debug-evidence/captures/bench_646910_goal_test1_20260923.cs8`.
- First clean, complete, correctly-terminated short-frame receptions
  (560793), immediately following the 50 ms settling-delay fix:
  `debug-evidence/captures/bench_646910_settle50ms_success_20260923.cs8` --
  same test run also independently confirms 646910 transmitted real replies
  the bridge still caught none of, ruling out "pump silent" for that run.
- AndroidAPS references inspected locally: RileyLink
  `RileyLinkCommunicationManager.wakeUp()` and `scanForDevice()`,
  `MedtronicCommunicationManager.createPumpMessageContent()`, and
  `RileyLinkTargetFrequency.kt`.

## Ingest guidance for another agent/model

Treat this file's front matter as authoritative constraints. Separate
observations from hypotheses. The bench serial is the only active target.
The real pump 560793 may be passively listened to, but never transmit to it.
Never use a generic ACK in a mixed capture as proof of a bench response.
The AndroidAPS-timed scan and parameterized RFPowerOn scan have both been run.
**As of Follow-up 12 (2026-09-24), the investigation is RESOLVED**: the bridge
reliably receives and correctly decodes bench pump 646910's real reply and
delivers it over BLE, confirmed by two independent ground-truth-backed test
runs. A prior session (2026-09-23, under the same standing goal) found and
fixed several real receive-path bugs -- see Follow-up 5 -- and got the bridge
receiving 560793 first; 646910 itself did not come through cleanly until
Follow-up 12. Do not re-open the receive-path investigation from scratch on
a fresh read of this file -- treat reception as working, and if a future test
shows 646910 not being received again, treat that as a regression against
this known-good baseline (check recent firmware changes first) rather than
restarting the original investigation. A `no BLE response` immediately after
a script that crashed instead of exiting cleanly is very likely the BlueZ
stale-GATT-cache issue documented in Follow-up 12, not a firmware fault --
rule that out first (`bluetoothctl remove <bridge MAC>`, no sudo needed)
before assuming reception has regressed. Any future active request must use
serial 646910 only and count only its CRC-valid, expected-opcode response.
