# ESP-VoCat music migration

The `feature/esp-vocat-migration` branch ports local MCP music playback to the current upstream
ESP-VoCat board. It intentionally keeps the upstream `AudioService` and `BoxAudioCodec` ownership
model: MP3 is decoded in a worker, converted to the codec's native 24 kHz mono format, and queued
through `AudioService::PushPcmToPlaybackQueue`.

The ESP-VoCat board exposes two MCP tools:

- `self.online_music.play_music`
- `self.online_music.stop_music`

Playback accepts the short-lived private-LAN `/media/<token>/audio` URL produced by the local music
MCP (plus Espressif's official test host). An optional `/media/<token>/manifest.json` URL supplies the
telemetry endpoint. The firmware emits playback lifecycle, underrun, wake, listening, STT, and TTS
events so server cache hits and network fetches use the same behavior-analysis path.

## Automated acceptance

```sh
source /Users/max/esp/esp-idf-v6.1/export.sh
python3 -m unittest discover -s scripts/tests -v
python3 scripts/build.py espressif/esp-vocat --name esp-vocat
```

Hardware acceptance must additionally cover play, stop, wake interruption, conversation resume,
natural completion, and a second playback served from the local music cache while monitoring the
serial log for I2S errors.
