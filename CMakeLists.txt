cmake_minimum_required(VERSION 3.22)

# Catalina Intel; never silently raise the deployment target.
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "macOS deployment target")
set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Target CPU architecture")

project(PlayStageCatalinaHost VERSION 0.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.16-security
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(juce)

juce_add_gui_app(PlayStageCatalinaHost
    PRODUCT_NAME "PlayStage VST3 Catalina"
    COMPANY_NAME "PlayStage Community"
    BUNDLE_ID "br.playstage.catalina.vst3host"
    PLUGINHOST_VST3 TRUE
    NEEDS_MICROPHONE_PERMISSION FALSE)

juce_generate_juce_header(PlayStageCatalinaHost)

target_sources(PlayStageCatalinaHost PRIVATE Source/Main.cpp)

target_compile_definitions(PlayStageCatalinaHost PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(PlayStageCatalinaHost PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
