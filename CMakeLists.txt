cmake_minimum_required(VERSION 3.24)
project(Bouncer VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.deps" CACHE PATH "")
include(FetchContent)
FetchContent_Declare(juce
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.15.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.59.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _header_only_no_cmake)
FetchContent_MakeAvailable(juce nlohmann_json httplib)
add_library(bc_httplib INTERFACE)
target_include_directories(bc_httplib SYSTEM INTERFACE ${httplib_SOURCE_DIR})

set(BC_ENGINE_SOURCES
    src/core/Params.cpp
    src/core/StateStore.cpp
    src/host/ReaperBridge.cpp
    src/dsp/Filters.cpp
    src/dsp/ChannelStrip.cpp
    src/dsp/Doubler.cpp
    src/dsp/Echo.cpp
    src/dsp/FdnReverb.cpp
    src/dsp/Limiter.cpp
    src/dsp/MixEngine.cpp
    src/analysis/Features.cpp
    src/analysis/Loudness.cpp
    src/analysis/Analyzer.cpp
    src/agent/Tools.cpp
    src/agent/PromptBuilder.cpp
    src/agent/LlmClient.cpp
    src/agent/ServerManager.cpp
    src/agent/Agent.cpp
    src/agent/AgentLog.cpp
    src/standalone/MultitrackPlayer.cpp
    src/PluginProcessor.cpp)

# The web UI, compiled into the binary (no files to install next to it).
juce_add_binary_data(bouncer_web SOURCES web/index.html)

juce_add_console_app(bouncer PRODUCT_NAME "bouncer")
target_sources(bouncer PRIVATE ${BC_ENGINE_SOURCES} app/main.cpp app/WebServer.cpp)
target_include_directories(bouncer PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/app)
target_include_directories(bouncer SYSTEM PRIVATE ${juce_SOURCE_DIR}/modules/juce_audio_processors_headless/format_types/VST3_SDK)
target_compile_definitions(bouncer PRIVATE
    JUCE_STANDALONE_APPLICATION=1 IM_MAX_TRACKS=64
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=1 CPPHTTPLIB_NO_EXCEPTIONS=0)
target_link_libraries(bouncer PRIVATE
    bouncer_web
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats juce::juce_dsp juce::juce_gui_extra
    nlohmann_json::nlohmann_json bc_httplib
    juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
