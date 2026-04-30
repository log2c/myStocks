cmake_minimum_required(VERSION 3.21)
project(MyStocks VERSION 0.0.1 LANGUAGES CXX)

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

if(WIN32 OR APPLE)
    include(FetchContent)
    FetchContent_Declare(
        QHotkey
        GIT_REPOSITORY https://github.com/Skycoder42/QHotkey.git
        GIT_TAG 1.5.0
    )
    FetchContent_MakeAvailable(QHotkey)
endif()

set(MYSTOCKS_SOURCES
    main.cpp
    resources.qrc
    src/app_logging.cpp
    src/app_controller.cpp
    src/config_manager.cpp
    src/crash_logging.cpp
    src/floating_window.cpp
    src/market_breadth_window.cpp
    src/i18n.cpp
    src/network_logger.cpp
    src/network_utils.cpp
    src/quote_model.cpp
    src/quote_provider.cpp
    src/settings_dialog.cpp
    src/updater.cpp
    src/watchlist_utils.cpp
)

set(MYSTOCKS_HEADERS
    include/app_constants.h
    include/app_logging.h
    include/app_controller.h
    include/config_manager.h
    include/crash_logging.h
    include/floating_window.h
    include/market_breadth_window.h
    include/i18n.h
    include/network_logger.h
    include/network_utils.h
    include/quote_model.h
    include/quote_provider.h
    include/settings_dialog.h
    include/types.h
    include/updater.h
    include/watchlist_utils.h
)

qt_add_executable(MyStocks
    ${MYSTOCKS_SOURCES}
    ${MYSTOCKS_HEADERS}
)

target_include_directories(MyStocks PRIVATE include)

target_link_libraries(MyStocks PRIVATE
    Qt6::Widgets
    Qt6::Network
)

if(WIN32 OR APPLE)
    target_link_libraries(MyStocks PRIVATE QHotkey::QHotkey)
endif()

if(WIN32)
    target_link_libraries(MyStocks PRIVATE Dbghelp)

    # Build as a GUI app so startup does not show a transient console window.
    set_target_properties(MyStocks PROPERTIES WIN32_EXECUTABLE TRUE)

    # Windows EXE file icon (Explorer/taskbar) from assets/icons/icon.ico.
    target_sources(MyStocks PRIVATE
        app_icon.rc
    )

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

target_compile_definitions(MyStocks PRIVATE
    APP_VERSION_STRING="${PROJECT_VERSION}"
    APP_GITHUB_URL="https://github.com/log2c/myStocks"
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(MyStocks PRIVATE DEBUG_MODE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

if(APPLE)
    set(MYSTOCKS_MAC_ICON_NAME "MyStocks.icns")
    set(MYSTOCKS_MAC_ICON_SOURCE "${CMAKE_SOURCE_DIR}/assets/icons/icon.png")
    set(MYSTOCKS_MAC_ICONSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/MyStocks.iconset")
    set(MYSTOCKS_MAC_ICON_ICNS "${CMAKE_CURRENT_BINARY_DIR}/${MYSTOCKS_MAC_ICON_NAME}")

    find_program(MYSTOCKS_SIPS_EXECUTABLE sips)
    find_program(MYSTOCKS_ICONUTIL_EXECUTABLE iconutil)

    if(NOT MYSTOCKS_SIPS_EXECUTABLE OR NOT MYSTOCKS_ICONUTIL_EXECUTABLE)
        message(FATAL_ERROR "sips and iconutil are required to generate the macOS app icon.")
    endif()

    add_custom_command(
        OUTPUT "${MYSTOCKS_MAC_ICON_ICNS}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${MYSTOCKS_MAC_ICONSET_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MYSTOCKS_MAC_ICONSET_DIR}"
        COMMAND "${MYSTOCKS_SIPS_EXECUTABLE}" -z 16 16 "${MYSTOCKS_MAC_ICON_SOURCE}" --out "${MYSTOCKS_MAC_ICONSET_DIR}/icon_16x16.png"
        COMMAND "${MYSTOCKS_SIPS_EXECUTABLE}" -z 32 32 "${MYSTOCKS_MAC_ICON_SOURCE}" --out "${MYSTOCKS_MAC_ICONSET_DIR}/icon_16x16@2x.png"
        COMMAND "${MYSTOCKS_SIPS_EXECUTABLE}" -z 32 32 "${MYSTOCKS_MAC_ICON_SOURCE}" --out "${MYSTOCKS_MAC_ICONSET_DIR}/icon_32x32.png"
        COMMAND "${MYSTOCKS_SIPS_EXECUTABLE}" -z 64 64 "${MYSTOCKS_MAC_ICON_SOURCE}" --out "${MYSTOCKS_MAC_ICONSET_DIR}/icon_32x32@2x.png"
        COMMAND "${MYSTOCKS_SIPS_EXECUTABLE}" -z 128 128 "${MYSTOCKS_MAC_ICON_SOURCE}" --out "${MYSTOCKS_MAC_ICONSET_DIR}/icon_128x128.png"
        COMMAND "${MYSTOCKS_SIPS_EXECUTABLE}" -z 256 256 "${MYSTOCKS_MAC_ICON_SOURCE}" --out "${MYSTOCKS_MAC_ICONSET_DIR}/icon_128x128@2x.png"
        COMMAND "${MYSTOCKS_SIPS_EXECUTABLE}" -z 256 256 "${MYSTOCKS_MAC_ICON_SOURCE}" --out "${MYSTOCKS_MAC_ICONSET_DIR}/icon_256x256.png"
        COMMAND "${MYSTOCKS_SIPS_EXECUTABLE}" -z 512 512 "${MYSTOCKS_MAC_ICON_SOURCE}" --out "${MYSTOCKS_MAC_ICONSET_DIR}/icon_256x256@2x.png"
        COMMAND "${MYSTOCKS_SIPS_EXECUTABLE}" -z 512 512 "${MYSTOCKS_MAC_ICON_SOURCE}" --out "${MYSTOCKS_MAC_ICONSET_DIR}/icon_512x512.png"
        COMMAND "${MYSTOCKS_SIPS_EXECUTABLE}" -z 1024 1024 "${MYSTOCKS_MAC_ICON_SOURCE}" --out "${MYSTOCKS_MAC_ICONSET_DIR}/icon_512x512@2x.png"
        COMMAND "${MYSTOCKS_ICONUTIL_EXECUTABLE}" -c icns "${MYSTOCKS_MAC_ICONSET_DIR}" -o "${MYSTOCKS_MAC_ICON_ICNS}"
        DEPENDS "${MYSTOCKS_MAC_ICON_SOURCE}"
        VERBATIM
    )

    add_custom_target(MyStocksMacIcon DEPENDS "${MYSTOCKS_MAC_ICON_ICNS}")
    add_dependencies(MyStocks MyStocksMacIcon)

    set_source_files_properties("${MYSTOCKS_MAC_ICON_ICNS}" PROPERTIES
        GENERATED TRUE
        MACOSX_PACKAGE_LOCATION "Resources"
    )

    target_sources(MyStocks PRIVATE
        "${MYSTOCKS_MAC_ICON_ICNS}"
    )

    set_target_properties(MyStocks PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "MyStocks"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.mystocks.app"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "${MYSTOCKS_MAC_ICON_NAME}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
    )
endif()

install(TARGETS MyStocks
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
