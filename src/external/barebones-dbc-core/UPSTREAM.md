# Barebones DB Core

Unmodified codec and schema sources from
https://github.com/Bar3b0n3s/Barebones-DBC-Editor (main, retrieved 2026-10-03).
The upstream MIT license is included in LICENSE.

Noggit uses only the table, DBD schema, and binary codec modules, not the GUI.
Saving retains the loaded container version and layout. Encrypted/opaque
sections are rejected rather than silently discarded.
