# spiffs/

Contents flashed into the `storage` partition (2 MB SPIFFS) via
`spiffs_create_partition_image` in `main/CMakeLists.txt`.

Currently empty. The chatgpt_demo seed shipped canned prompts here
("Please wait a moment", chimes, TTS-failed fallback) — all removed as
part of the HavenCore MVP, since the device now relies purely on the
visual state panels and the live TTS stream from the server for
feedback.

Kept as a placeholder so the partition image still builds, and so any
future locally-stored assets (e.g. offline boot chime, error tone) have
an obvious home.
