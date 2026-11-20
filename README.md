# MIDI Transport

MIDI Transport is a silent instrument plugin for the MPC X. It learns MIDI CC or note assignments for **Play**, **Stop**, and **Play Start** and saves them in the project. It uses the Open MPC Remote add-in on the same MPC to press its global transport buttons.

## Use

1. Install the plugin folder in `VST2 Plugins` on the plugin drive. The MPC needs firmware 3.10.1 or later, External Plugin Support, and Open MPC Remote with `PLAY`, `STOP`, and `PLAY-START` available on its local port 6720.
2. Load **Elsewave → MIDI Transport** on a plugin instrument track. In the track's **I/O** strip, choose your controller under **MIDI In** and set **Monitor** to **In**.
3. Open the plugin's **CONTROL** page. Tap **Learn** beside an action, then send a MIDI CC or play a note. **Listening** changes back to **Learn** when the assignment is captured, and the MIDI channel and message appear beside the button. Tap **Unassign** to clear the mapping.
4. Notes trigger on Note On. Use momentary CC buttons that send 127 on press and 0 on release. The learned action triggers on a transition into values 64–127. The learning message itself does not trigger transport. Save the MPC project to keep the mappings.

The plugin receives the MIDI channel and CC as delivered by the MPC track. CC events played back on that track can also trigger transport.

On an original MPC X running 3.10.1.12, incoming test CCs triggered Play, Stop, and Play Start. Saving and restoring the learned mappings was tested in the local VST host.

## Build

Set `MPC_SDK` to the Akai MPC Plugin Builder SDK and run `sh build.sh`. Set `ZIG` or `PYTHON` to select the installed tools.
