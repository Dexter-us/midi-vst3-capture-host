cmake_minimum_required(VERSION 3.25)

project(MidiVst3CaptureHost VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(MidiVst3CaptureHost
    PRODUCT_NAME "MIDI VST3 Capture Host"
    COMPANY_NAME "Dexter U.S."
    BUNDLE_ID "com.dexterus.midivst3capturehost"
)

target_sources(MidiVst3CaptureHost PRIVATE
    src/MidiVst3CaptureHost.cpp
)

target_compile_features(MidiVst3CaptureHost PRIVATE cxx_std_17)

target_compile_definitions(MidiVst3CaptureHost PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

target_link_libraries(MidiVst3CaptureHost PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)