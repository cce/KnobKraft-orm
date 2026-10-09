# kk — KnobKraft from the command line

`kk` is a small command-line client for a **running** KnobKraft Orm. It talks
to the application through the same local session bridge the KnobKraft Recall
plugin uses (see `MidiKraft/session/PROTOCOL.md`). KnobKraft does all the real
work (patch database, synth adaptations, MIDI), so every synth KnobKraft
supports works the same way from `kk`, native and Python adaptations alike.

## Usage

```text
kk info                                   server version
kk synths                                 configured synths, online state, capabilities
kk search pad --synth Rev2                patches whose name contains "pad"
kk show "DSI Prophet Rev2:3f2a…"          one patch
kk send --search "Warm Strings"           send to the matching synth's edit buffer
kk send "DSI Prophet Rev2:3f2a…" --synth Rev2
kk status <transferId>  /  kk cancel <transferId>
kk open --synth Rev2                      bring the KnobKraft window to the front
```

- `--synth` accepts a configured-synth id, a synth name, or any part of one.
- `send` waits for the transfer to finish and prints progress; `--no-wait`
  prints the transfer and returns at once.
- Every command accepts `--json` for scripting.
- Patch ids contain spaces (`<synth name>:<md5>`), so quote them.

Exit codes: `0` ok, `1` usage, `2` KnobKraft reported an error, `3` KnobKraft
is not running (or its bridge is inactive), `4` the transfer failed or was
cancelled.

`kk` finds KnobKraft through the discovery file it publishes in the per-user
application-data folder (`~/Library/KnobKraftOrm/recall-session-v1.json` on
macOS, `%APPDATA%\KnobKraftOrm\…` on Windows, `~/.config/KnobKraftOrm/…` on
Linux). Override it with `--discovery-file` or `KK_DISCOVERY_FILE`.

Sends show up in KnobKraft's Plugin Sessions drawer as coming from `kk`.

## Building

`kk` is built with the rest of KnobKraft (`BUILD_KK_CLI`, on by default). It
only needs `MidiKraft/session` and nlohmann/json, so it can also be built on
its own, without JUCE:

```sh
cmake -S KnobKraft-CLI -B build-kk -DBUILD_KK_TESTS=ON
cmake --build build-kk
ctest --test-dir build-kk
```

The tests run the real CLI against an in-process session server backed by
MidiKraft's `FakeSessionService`, so they need no running KnobKraft or synth.

## Limits

`kk` can only do what the session protocol offers: list synths, search and
fetch patches, send to the edit buffer, and track transfers. Importing from a
synth, bank dumps, and export would need new operations on the KnobKraft side.
