import unittest
import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class EspVocatMusicMigrationTests(unittest.TestCase):
    def read(self, relative_path):
        return (ROOT / relative_path).read_text(encoding="utf-8")

    def test_music_uses_serialized_audio_service_path(self):
        music = self.read("main/boards/common/esp32_music.cc")
        service = self.read("main/audio/audio_service.cc")

        self.assertIn("PushPcmToPlaybackQueue", music)
        self.assertIn("audio_playback_queue_.push_back", service)
        self.assertNotIn("SetOutputSampleRate", music)
        self.assertNotIn("codec_->OutputData", music)

    def test_music_closes_initial_listening_session_before_playback(self):
        music = self.read("main/boards/common/esp32_music.cc")

        self.assertIn("Closing listening session before music playback", music)
        self.assertIn("app.ToggleChatState()", music)
        self.assertLess(
            music.index("app.ToggleChatState()"),
            music.index("PushPcmToPlaybackQueue"),
        )

    def test_repeat_play_does_not_query_pthread_identity_from_main_task(self):
        music = self.read("main/boards/common/esp32_music.cc")

        self.assertIn("bool Esp32Music::RecreateDecoder()", music)
        self.assertIn("esp_audio_simple_dec_close(decoder_)", music)
        self.assertNotIn("esp_audio_simple_dec_reset(decoder_)", music)
        self.assertNotIn("std::this_thread::get_id()", music)

    def test_esp_vocat_registers_play_and_stop_tools(self):
        board = self.read("main/boards/espressif/esp-vocat/esp_vocat.cc")

        self.assertEqual(board.count("DECLARE_BOARD(EspVocat)"), 1)
        self.assertIn('"self.online_music.play_music"', board)
        self.assertIn('"self.online_music.stop_music"', board)
        self.assertIn('IsProxyUrl(manifest, "manifest.json")', board)
        self.assertIn("IsApprovedAudioUrl(url)", board)

    def test_playback_and_conversation_are_attributed(self):
        music = self.read("main/boards/common/esp32_music.cc")
        application = self.read("main/application.cc")

        for event in ("playback_started", "playback_completed", "playback_stopped"):
            self.assertIn(event, music)
        for event in (
            "wake_detected",
            "listening_started",
            "listening_stopped",
            "user_utterance",
            "assistant_response",
        ):
            self.assertIn(event, application if event != "playback_started" else music)

    def test_upstream_codec_is_not_reimplemented_for_music(self):
        codec = self.read("main/audio/codecs/box_audio_codec.cc")
        music = self.read("main/boards/common/esp32_music.cc")

        self.assertIn("std::lock_guard<std::mutex>", codec)
        self.assertNotIn("i2s_channel_write", music)
        self.assertNotIn("EnableOutput", music)

    def test_esp_vocat_exposes_cat_actions_and_music_companion(self):
        board = self.read("main/boards/espressif/esp-vocat/esp_vocat.cc")
        display = self.read("main/boards/espressif/esp-vocat/vocat_cat_display.cc")
        display_header = self.read("main/boards/espressif/esp-vocat/vocat_cat_display.h")
        mcp = self.read("main/mcp_server.cc")

        self.assertIn("new emote::VocatCatDisplay", board)
        self.assertIn("RenderMusicCompanion", display)
        self.assertIn("CharacterActionRequest", display_header)
        self.assertLess(
            display.index('"vocat_cat_character"'),
            display.index("EmoteDisplay::LoadAssets();"),
        )
        self.assertNotIn("vocat_cat_subtitle", display)
        self.assertIn("EmoteDisplay::SetChatMessage(role, content)", display)
        self.assertNotIn("EmoteDisplay::SetStatus(status)", display)
        self.assertNotIn("EmoteDisplay::SetEmotion(emotion)", display)
        self.assertIn("EMT_DEF_ELEM_LISTEN_ANIM, false", display)
        self.assertIn("action_request_.Tick(now, true, false)", display)
        self.assertIn('"self.screen.perform_cat_action"', mcp)
        self.assertIn('"self.music.set_companion_instrument"', mcp)

    def test_cat_renderer_action_lifecycle_and_preferences(self):
        source = r'''
#include "character_action.h"
#include "music_companion_preferences.h"
#include <cassert>
#include <cstdint>
#include <vector>
int main() {
    using namespace anim;
    std::vector<uint8_t> frame(kCharacterBytes);
    for (int scene = 0; scene <= static_cast<int>(CharacterPreview::kSurprised); ++scene) {
        assert(RenderCharacterPreview(frame.data(), frame.size(),
                                      static_cast<CharacterPreview>(scene), 1.25f));
    }
    CharacterActionRequest request;
    assert(request.Queue("heart", 0));
    assert(!request.Tick(1, false, false));
    assert(request.Tick(2, true, false) == CharacterPreview::kHeart);
    assert(!request.Tick(3, false, false));
    assert(request.Queue("wave", 10));
    assert(!request.Tick(30000010, true, false));
    MusicCompanionPreferences preferences;
    assert(preferences.Update("cat", "guitar"));
    assert(MusicCompanionPreferences::Decode(preferences.Encode()).instrument ==
           CharacterPreview::kGuitar);
    assert(RenderMusicCompanion(frame.data(), frame.size(), 2.0,
                                CharacterPreview::kDrum));
}
'''
        board_dir = ROOT / "main/boards/espressif/esp-vocat"
        with tempfile.TemporaryDirectory() as directory:
            test_source = Path(directory) / "cat_test.cc"
            executable = Path(directory) / "cat_test"
            test_source.write_text(source, encoding="utf-8")
            subprocess.run(
                [
                    os.environ.get("CXX", "c++"),
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{board_dir}",
                    str(test_source),
                    str(board_dir / "character_preview.cc"),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
