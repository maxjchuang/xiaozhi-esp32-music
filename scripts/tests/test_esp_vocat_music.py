import unittest
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


if __name__ == "__main__":
    unittest.main()
