# Delivery roadmap

The following work items are created as GitHub Issues. They are intentionally ordered by dependency, but a later control-domain task may proceed once its required protocol contract is stable.

| Phase | GitHub Issue | Completion condition |
| --- | --- | --- |
| 0 | Foundation and design baseline | Project docs, contributor constraints, repository scaffold, and issue roadmap are committed. |
| 1 | Pico USB HID keyboard device | A PC enumerates Pico as a keyboard and accepts a fixed release/key report. |
| 2 | Pico PIO USB host and capture | Boot Keyboard reports are received and stamped with Pico hardware time. |
| 3 | Pico pass-through and UART record mirror | PASS forwards physical reports; RECORD emits Pico-timestamped version-2 `RECORD_EVENT` frames. |
| 4 | UART mode control and safety | UART0 RX ring, main-loop version/CRC parser, `MODE_SET`, `MODE_CHANGED`, and all-release transitions are verified. |
| 5 | Zero recorder CLI and persistence | Capture sessions persist valid recordings with list/dump operations. |
| 6 | Version-2 UART parser and Pico playback queue | Framing, CRC, queue commands/status, and queue tests operate correctly. |
| 7 | Pico absolute-deadline playback scheduler | Pico drives HID reports from a Pico-owned epoch and publishes lateness metrics. |
| 8 | Zero playback feeder, prebuffer, and streaming | Future events are supplied with flow control and no Zero-local playback timer. |
| 9 | Reliability and RT test suite | Underrun, disconnect, watchdog, load/stall, and jitter tests pass. |
| 10 | Zero web API and service lifecycle | FastAPI controls recording/playback and systemd shutdown is safe. |
| 11 | Web UI and recording management | Browser UI supports status, record/stop/play/abort, rename, and delete. |
| 12 | Integrated acceptance and release readiness | All documented functional and RT acceptance tests pass on hardware. |

Each implementation issue must preserve the invariants in [AGENT.md](../AGENT.md), document protocol changes in [protocol.md](protocol.md), and add its practical verification method to the README/testing documentation.
