cmake_minimum_required(VERSION 3.21)
project(MyStocks VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

if(APPLE)
    if(NOT DEFINED CMAKE_PREFIX_PATH AND NOT DEFINED ENV{CMAKE_PREFIX_PATH})
        set(_qt6_prefix_candidates
            "/opt/homebrew/opt/qt"
            "/usr/local/opt/qt"
        )

        foreach(_qt6_prefix IN LISTS _qt6_prefix_candidates)
            if(EXISTS "${_qt6_prefix}/lib/cmake/Qt6/Qt6Config.cmake")
                list(PREPEND CMAKE_PREFIX_PATH "${_qt6_prefix}")
                message(STATUS "Using Homebrew Qt6 from ${_qt6_prefix}")
                break()
            endif()
        endforeach()
    endif()
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

if(WIN32)
    include(FetchContent)
    FetchContent_Declare(
        QHotkey
        GIT_REPOSITORY https://github.com/Skycoder42/QHotkey.git
        GIT_TAG 1.5.0
    )
    FetchContent_MakeAvailable(QHotkey)
endif()

qt_add_executable(MyStocks
    main.cpp
    src/app_logging.cpp
    src/app_controller.cpp
    src/config_manager.cpp
    src/floating_window.cpp
    src/i18n.cpp
    src/network_logger.cpp
    src/network_utils.cpp
    src/quote_model.cpp
    src/quote_provider.cpp
    src/settings_dialog.cpp
    include/app_logging.h
    include/app_controller.h
    include/config_manager.h
    include/floating_window.h
    include/i18n.h
    include/network_logger.h
    include/network_utils.h
    include/quote_model.h
    include/quote_provider.h
    include/settings_dialog.h
    include/types.h
)

target_include_directories(MyStocks PRIVATE include)

target_link_libraries(MyStocks PRIVATE
    Qt6::Widgets
    Qt6::Network
)

if(WIN32)
    # Build as a GUI app so startup does not show a transient console window.
    set_target_properties(MyStocks PROPERTIES WIN32_EXECUTABLE TRUE)

    target_link_libraries(MyStocks PRIVATE QHotkey::QHotkey)

    if(MSVC)
        # Use static runtime (/MT) for Release to reduce redistribution footprint.
        # Keep Debug on /MDd for better debugging experience.
        set(_mystocks_msvc_runtime "MultiThreaded$<$<CONFIG:Debug>:DebugDLL>")

        set_target_properties(MyStocks PROPERTIES
            MSVC_RUNTIME_LIBRARY "${_mystocks_msvc_runtime}"
        )

        if(TARGET qhotkey)
            set_target_properties(qhotkey PROPERTIES
                MSVC_RUNTIME_LIBRARY "${_mystocks_msvc_runtime}"
            )
            target_compile_options(qhotkey PRIVATE /EHsc)
        endif()

        target_compile_options(MyStocks PRIVATE
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/Gw>
        )
        target_link_options(MyStocks PRIVATE
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
        )
    elseif(MINGW)
        target_link_options(MyStocks PRIVATE
            $<$<CONFIG:Release>:-s>
            $<$<CONFIG:Release>:-static-libgcc>
            $<$<CONFIG:Release>:-static-libstdc++>
        )
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(MyStocks PRIVATE DEBUG_MODE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

if(APPLE)
    set_target_properties(MyStocks PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "MyStocks"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.mystocks.app"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
    )
endif()

install(TARGETS MyStocks
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
