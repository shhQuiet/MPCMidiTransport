# MIDI Transport

MIDI Transport is a silent instrument plugin for the MPC X. It learns MIDI CC or note assignments for MPC hardware buttons and saves them in the project. It uses the Open MPC Remote add-in running on the same MPC to send the button presses.

The ARMv7 build was tested on an original MPC X. The AArch64 build is available for Gen2 and awaits hardware testing.

## Use

1. Install the plugin folder in `VST2 Plugins` on the plugin drive. The MPC needs firmware 3.10.1 or later, External Plugin Support, and [Open MPC Remote](https://github.com/jacob-sabella/mpc-addin-remote) on local port 6720. Open MPC Remote needs to know the buttons you choose.
2. Load **Elsewave → MIDI Transport** on a plugin instrument track. In the track's **I/O** strip, choose your controller under **MIDI In** and set **Monitor** to **In**.
3. Open **CONTROL** and tap **+** to add a row. Tap **Choose control** and select an MPC button. Tap **Learn**, then press a MIDI CC button or play a note. The row shows the captured channel and message and returns to **Learn**. Tap **Unassign** to clear its MIDI message or **−** to remove the row. The page buttons appear once there are more than eight rows.
4. Selecting a different MPC action sets the row's initial mode. **FULL-LEVEL** and **16-LEVEL** start on **Toggle**. Other actions start on **Momentary**. Tap the mode button to match your controller. Use Momentary when the button sends on at press and off at release. Use Toggle when successive presses alternate between on and off. In Toggle mode, either transition triggers the action. CC values 64–127 count as on. The message captured by Learn does not trigger the action. Save the MPC project to keep the mappings and their modes.

The plugin receives the MIDI channel and CC as delivered by the MPC track. CC events played back on that track can also trigger the assigned action.

The chooser includes 33 button commands. MIDI Transport sends short button presses, and the Open MPC Remote button profile determines which physical buttons each command operates on your model. The QLINK entry is a button press. It does not turn a knob. Controls that require a hold or a button combination may have limited behavior.

## Build

Set `MPC_SDK` to the Akai MPC Plugin Builder SDK and run `sh build.sh`. Set `ZIG` or `PYTHON` to select the installed tools.

To test the native build on macOS or Linux, compile `tests/host_smoke.cpp` with a C++17 compiler and `-I "$MPC_SDK/framework" -ldl`, then run the resulting executable with the native plugin from `build/native` as its argument.

MIDI Transport is MIT licensed. The Akai SDK dependency has its own license.
