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

    set_target_properties(QHotkey PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    )
    target_compile_options(QHotkey PRIVATE /EHsc)
endif()

qt_add_executable(MyStocks
    main.cpp
    src/app_controller.cpp
    src/config_manager.cpp
    src/floating_window.cpp
    src/i18n.cpp
    src/network_logger.cpp
    src/network_utils.cpp
    src/quote_model.cpp
    src/quote_provider.cpp
    src/settings_dialog.cpp
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
    target_link_libraries(MyStocks PRIVATE QHotkey::QHotkey)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(MyStocks PRIVATE DEBUG_MODE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

if(APPLE)
    set_target_properties(MyStocks PROPERTIES MACOSX_BUNDLE TRUE)
endif()

install(TARGETS MyStocks
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
