cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
project(BandwidthCrusher VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE is downloaded at configure time. To use a local copy instead:
#   cmake -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE ...
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

if(APPLE)
    set(BC_FORMATS AU VST3 Standalone)
else()
    set(BC_FORMATS VST3 Standalone)
endif()

juce_add_plugin(BandwidthCrusher
    PRODUCT_NAME                "Bandwidth Crusher"
    COMPANY_NAME                "Bytebent Audio"
    BUNDLE_ID                   "com.bytebent.bandwidthcrusher"
    PLUGIN_MANUFACTURER_CODE    Bytb
    PLUGIN_CODE                 Bwcr
    FORMATS                     ${BC_FORMATS}
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    NEEDS_WEB_BROWSER           TRUE
    NEEDS_CURL                  FALSE
    AU_MAIN_TYPE                kAudioUnitType_Effect
    VST3_CATEGORIES             Fx Distortion
    COPY_PLUGIN_AFTER_BUILD     FALSE)

juce_add_binary_data(BandwidthCrusherUI SOURCES ui/index.html)

target_sources(BandwidthCrusher PRIVATE
    source/PluginProcessor.cpp
    source/PluginEditor.cpp)

target_compile_definitions(BandwidthCrusher PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(BandwidthCrusher
    PRIVATE
        BandwidthCrusherUI
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Command-line test runner for the DSP core (see tests/compare.py).
add_executable(dsp_cli tests/dsp_cli.cpp)
