# WASMIDI Pass 8.2.1 — horizontal visualizer restore

Apply on top of Pass 8.2.

Pass 8.2 split the renderer into:
- completedVao_/completedVbo_: main ring of completed notes
- openVao_/openVbo_: at most 2048 currently held states

The open-note VAO was fully configured, but completedVao_ was only generated.
Its StartTick, EndTick and PackedData arrays were never enabled and their
divisors were never set.

As a result, glDrawArraysInstanced() for the completed ring consumed constant
zero vertex attributes and almost all normal MIDI notes were invisible.

This overlay initializes completedVao_ with the same 12-byte RenderNote layout:
- location 0: int StartTick, divisor 1
- location 1: int EndTick, divisor 1
- location 2: uint PackedData, divisor 1

No parser, scheduler, layout, QML, background, keyboard, timing, color or FPS
optimization is changed.
