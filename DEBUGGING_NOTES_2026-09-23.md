---
record_type: hardware_debugging_handoff
date: 2026-09-23
project: OrangeLink xG28 / Medtronic 722 radio link
status: active
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

## Current conclusion

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

## Next steps

1. Find why RAIL does not capture/deliver the 646910 frames that HackRF decoded
   during the same run. Correlate RTT radio events and actual RX tune with the
   response carrier; then fix and reflash the xG28 driver.
2. If hardware capture resumes, record the 916.670–916.730 MHz fine scan at the
   same time on HackRF to determine whether reply carriers move with each
   requested frequency.
3. Preserve the foreign-frame filter. Only a valid-CRC frame with serial
   646910 counts as a bench response.
4. Large `.cs8` captures stay in the local `debug-evidence/captures/` directory
   and are excluded from Git by `.gitignore`; never use `/tmp`.

Suggested active command (use this interpreter path, BLE device, and bench
serial):

```sh
/home/charles/ai/orangelink-ncs-ws/.venv/bin/python /home/charles/ai/tools/rf/probe_722_aaps.py ClaudeLinkSI --serial 646910 --centre 916.625 --span 0.175 --step 0.05 --wake-timeout-ms 25000 --scan-timeout-ms 1250 --scan-tries 3
```

Passive HackRF capture for 90 seconds:

```sh
hackrf_transfer -r /home/charles/ai/orangelink-xg28/debug-evidence/captures/bench_646910_rfpoweron_new_run.cs8 -f 916100000 -s 2000000 -a 1 -l 32 -g 30 -n 180000000
```

## Evidence files and relevant source

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
The user requested no further tests at this wrap-up. If work resumes, investigate
why the xG28 receiver missed 646910's HackRF-confirmed model replies. Any future
active request must use serial 646910 only and count only its CRC-valid,
expected-opcode response.
