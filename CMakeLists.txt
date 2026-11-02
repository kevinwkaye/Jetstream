cmake_minimum_required(VERSION 3.22)

project(Jetstream VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
endif()

# ---------------------------------------------------------------------------
# JUCE: use a local copy if you have one (-DJETSTREAM_JUCE_PATH=...),
# otherwise download JUCE 8 automatically.
# ---------------------------------------------------------------------------
set(JETSTREAM_JUCE_PATH "" CACHE PATH "Path to a local JUCE checkout (optional)")
option(JETSTREAM_BUILD_PLUGIN "Build the plugin (needs JUCE)" ON)
option(JETSTREAM_BUILD_TESTS "Build the offline render test" ON)

if(JETSTREAM_BUILD_PLUGIN)
    if(JETSTREAM_JUCE_PATH)
        add_subdirectory(${JETSTREAM_JUCE_PATH} JUCE)
    else()
        include(FetchContent)
        FetchContent_Declare(
            JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 8.0.4
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(JUCE)
    endif()

    set(JETSTREAM_FORMATS VST3 Standalone)
    if(APPLE)
        list(APPEND JETSTREAM_FORMATS AU)
    endif()

    juce_add_plugin(Jetstream
        COMPANY_NAME "Kevin Kaye"
        COMPANY_WEBSITE ""
        BUNDLE_ID "uk.kevinkaye.jetstream"
        PLUGIN_MANUFACTURER_CODE Kkay
        PLUGIN_CODE Jets
        FORMATS ${JETSTREAM_FORMATS}
        PRODUCT_NAME "Jetstream"
        DESCRIPTION "Resynthesiser: sampled sustain, synth, glitch and swell voices"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        VST3_CATEGORIES Fx Delay Reverb)

    juce_add_binary_data(JetstreamData SOURCES Resources/RobotoCondensed-Bold.ttf)

    target_sources(Jetstream PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)

    target_include_directories(Jetstream PRIVATE Source Source/engine)

    target_compile_definitions(Jetstream PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_link_libraries(Jetstream
        PRIVATE
            JetstreamData
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()

# ---------------------------------------------------------------------------
# Offline engine test (no JUCE needed): renders test audio through every voice
# ---------------------------------------------------------------------------
if(JETSTREAM_BUILD_TESTS)
    add_executable(jetstream_render_test Tests/RenderTest.cpp)
    target_include_directories(jetstream_render_test PRIVATE Source/engine)
endif()
