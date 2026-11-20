#!/bin/sh
set -eu

: "${MPC_SDK:?Set MPC_SDK to an Akai MPC Plugin Builder SDK checkout}"
: "${ZIG:=zig}"
: "${PYTHON:=python3}"

project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export MIDI_TRANSPORT_ZIG
MIDI_TRANSPORT_ZIG=$(command -v "$ZIG")
mkdir -p "$project/build" "$project/Content"
cp "$MPC_SDK/LICENSE" "$project/Content/Akai-SDK-LICENSE.txt"

compiler="$project/build/zig-sdk"
cat > "$compiler" <<'SH'
#!/bin/sh
if [ "$1" = c++ ]; then
    shift
    for arg in "$@"; do
        if [ "$arg" = -target ]; then
            # Keep the ARM C++ runtime resident when MPC unloads a library.
            exec "$MIDI_TRANSPORT_ZIG" c++ "$@" -Wl,-z,nodelete
        fi
    done
    if [ "$(uname -s)" = Darwin ]; then
        exec /usr/bin/clang++ "$@"
    fi
    exec "$MIDI_TRANSPORT_ZIG" c++ "$@"
fi
exec "$MIDI_TRANSPORT_ZIG" "$@"
SH
chmod +x "$compiler"
"$PYTHON" "$MPC_SDK/tools/build.py" "$project" --target "${1:-all}" --zig "$compiler"
"$PYTHON" "$project/ui/render_control.py" "$MPC_SDK" "$project"
"$PYTHON" "$MPC_SDK/tools/validate.py" "$project" --strict
"$PYTHON" "$MPC_SDK/tools/package.py" "$project"
