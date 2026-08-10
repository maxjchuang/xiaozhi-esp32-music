#ifndef DISPLAY_BEHAVIOR_H
#define DISPLAY_BEHAVIOR_H

#include <string>

enum class DisplayBehavior {
    kStartup,
    kConnecting,
    kIdle,
    kWakeAcknowledged,
    kListening,
    kThinking,
    kToolRunning,
    kSpeaking,
    kSuccess,
    kRecoverableError,
    kFatalError,
    kMusicBuffering,
    kMusicPlaying,
    kMusicPaused,
};

enum class DisplayBehaviorSource {
    kDeviceState,
    kWakeWord,
    kTts,
    kMcp,
    kMusic,
    kNetwork,
    kSystem,
    kTimer,
};

struct DisplayBehaviorRequest {
    DisplayBehavior behavior;
    DisplayBehaviorSource source;
    std::string detail;
    int duration_ms = 0;
};

#endif  // DISPLAY_BEHAVIOR_H
