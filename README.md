# MIDI VST3 Capture Host

A small Windows JUCE host helper for MIDI-producing VST3 plugins. It opens a
plugin's editor, optionally feeds it a source MIDI file, captures MIDI events
returned by the plugin, and writes a Standard MIDI File.

This is an early prototype, not a general-purpose DAW or a security sandbox.
Only use VST3 plugins you trust. A plugin runs in-process and can crash the
helper.

## Build

The Windows GitHub Actions workflow builds a 64-bit Release executable and
uploads it as an artifact. For a local build, install CMake 3.25+ and Visual
Studio 2022 C++ tools, then run:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

JUCE 8.0.6 is fetched by CMake at configure time.

## Launch configuration

The helper accepts the path to a JSON configuration file as its only argument:

```json
{
  "plugin_path": "C:/Program Files/Common Files/VST3/Example.vst3",
  "input_midi_path": "C:/temp/source.mid",
  "output_midi_path": "C:/temp/captured.mid",
  "status_path": "C:/temp/capture-status.json",
  "duration_seconds": 15,
  "ticks_per_beat": 480,
  "tempo_microseconds": 500000
}
```

`input_midi_path` may be an empty string when the plugin generates notes
without MIDI input. The plugin editor opens first; configure it, then click
**Start capture**. The helper writes a success or failure result to
`status_path`. It will not write a successful MIDI file unless at least one
note-on event was captured.

The prototype feeds MIDI events from the input file in real time and timestamps
captured plugin output against the audio callback. It does not yet provide a
full DAW transport or isolate plugin crashes.