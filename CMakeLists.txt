cmake_minimum_required(VERSION 3.22)
set(FERMATA_VERSION 2.7.0)        # the ONE place the version number is changed (see CHANGELOG.md)
project(Fermata VERSION ${FERMATA_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static runtime on Windows so the .exe runs on any PC without installing the Visual C++ redistributable.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.9
    GIT_SHALLOW    ON)
FetchContent_MakeAvailable(JUCE)

# Path to the Steinberg ASIO SDK (the folder that contains "common", "host" and "driver").
set(ASIO_SDK_DIR "" CACHE PATH "Steinberg ASIO SDK folder")

juce_add_gui_app(Fermata
    PRODUCT_NAME "Fermata"
    COMPANY_NAME "Fermata"
    VERSION "${FERMATA_VERSION}"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/fermata-icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/assets/fermata-icon.png")

target_sources(Fermata PRIVATE
    src/Main.cpp
    src/core/Project.cpp
    src/core/AudioEngine.cpp
    src/core/Playback.cpp
    src/core/Bounce.cpp
    src/core/AudioOps.cpp
    src/core/ImportPlan.cpp
    src/core/MasterDef.cpp
    src/core/Ddp.cpp
    src/core/MasterExport.cpp
    src/core/MasterRender.cpp
    src/core/Ravenna.cpp
    src/core/CoreReservation.cpp
    src/reharmoniser/Dsp.cpp
    src/reharmoniser/Engine.cpp
    src/reharmoniser/Spectrogram.cpp
    src/reharmoniser/ReHarmoniserEditor.cpp
    src/ui/AppContext.cpp
    src/ui/BounceWindow.cpp
    src/ui/DesignWindow.cpp
    src/ui/EditWindow.cpp
    src/ui/FixTools.cpp
    src/ui/MasteringWindow.cpp
    src/ui/FontData.cpp
    src/ui/MainWindow.cpp
    src/ui/MediaWindow.cpp
    src/ui/WinOwner.cpp
    src/ui/TakeDisplay.cpp
    src/ui/ScopeWindow.cpp
    src/ui/OrganiserWindow.cpp
    src/ui/MixerWindow.cpp
    src/ui/PluginHost.cpp
    src/ui/RemoteControl.cpp
    src/ui/TakeWindow.cpp
    src/ui/TrimWindow.cpp)

target_compile_definitions(Fermata PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    FERMATA_VERSION_STRING="${FERMATA_VERSION}")

if(MSVC)
    # keep debugging symbols so a crash log can show exactly where it happened; the .pdb is copied next to the exe
    target_compile_options(Fermata PRIVATE /Zi)
    target_link_options(Fermata PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    add_custom_command(TARGET Fermata POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist/Fermata"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:Fermata>" "${CMAKE_SOURCE_DIR}/dist/Fermata/")
endif()

if(WIN32)
    if(NOT ASIO_SDK_DIR OR NOT EXISTS "${ASIO_SDK_DIR}/common/iasiodrv.h")
        message(FATAL_ERROR "Set ASIO_SDK_DIR to the Steinberg ASIO SDK folder (it must contain common/iasiodrv.h)")
    endif()
    target_compile_definitions(Fermata PRIVATE JUCE_ASIO=1 JUCE_WASAPI=1 JUCE_DIRECTSOUND=0)
    target_include_directories(Fermata PRIVATE
        "${ASIO_SDK_DIR}/common" "${ASIO_SDK_DIR}/host" "${ASIO_SDK_DIR}/host/pc")
endif()

target_link_libraries(Fermata PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_data_structures
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
