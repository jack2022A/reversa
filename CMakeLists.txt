cmake_minimum_required(VERSION 3.22)
project(StageBridgePlayStage VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Target Catalina")
  set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Mac Intel")
  set(FORMATS VST3 AU Standalone)
else()
  set(FORMATS VST3 Standalone)
endif()
include(FetchContent)
FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.16-security GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(juce)
juce_add_plugin(StageBridgeUnified
  COMPANY_NAME "PlayStage Community"
  PRODUCT_NAME "StageBridge PlayStage"
  BUNDLE_ID "br.playstage.stagebridge.unified"
  PLUGIN_MANUFACTURER_CODE PlSt
  PLUGIN_CODE SbPu
  IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  COPY_PLUGIN_AFTER_BUILD FALSE
  FORMATS ${FORMATS})
juce_generate_juce_header(StageBridgeUnified)
target_sources(StageBridgeUnified PRIVATE Source/StageBridgeUnified.cpp)
target_compile_definitions(StageBridgeUnified PRIVATE
  JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(StageBridgeUnified PRIVATE
  juce::juce_audio_utils juce::juce_audio_formats
  juce::juce_audio_processors juce::juce_gui_extra
  juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
