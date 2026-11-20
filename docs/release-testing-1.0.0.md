# MIDI Transport 1.0.0 release testing

The ARMv7 release was tested on an original MPC X running MPC OS 3.10.1.12 on November 20, 2026.

- The release installer put the ARMv7 package in `VST2 Plugins` on MPCSSD using a scratch copy of the app settings. After restart the MPC loaded the package binary from MPCSSD. Both installer packages also passed install, reinstall, and uninstall tests against isolated settings and folders on MPCSSD.
- The CONTROL page opened and displayed three mapping rows with the expected defaults and overrides. MIDI Learn captured a CC and a note. The learned Momentary CC triggered PLAY-START. Both transitions of a learned Toggle CC triggered STOP, and a repeat of the same CC state left playback running.
- A scratch project saved to MPCSSD reopened with all three actions, MIDI assignments, and modes intact.
- The Open MPC benchmark used 128-frame audio blocks at 44.1 kHz and measured **0.5% p99** and **3.3% maximum** of the audio block budget. The benchmark ran in a separate network namespace so its test input could not control the active MPC app.
- The native VST smoke test passed MIDI Learn, row removal, and project-state recall. It also confirmed that reopening resets the temporary Learn and chooser states. The generic SDK simulator reported silence and expected those temporary controls to persist across its state round-trip.
- Both catalog ZIPs passed the Open MPC package checker. The network warning refers to the plugin connecting to Open MPC Remote at `127.0.0.1:6720`. The AArch64 package passed build and package checks and awaits a Gen2 hardware test.
