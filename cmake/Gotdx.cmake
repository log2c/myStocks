find_program(MYSTOCKS_GO_EXECUTABLE go REQUIRED)
set(MYSTOCKS_GOTDX_REF "main" CACHE STRING "gotdx Git branch, tag or commit")
set(MYSTOCKS_GOTDX_SOURCE_DIR "" CACHE PATH "Use a local gotdx checkout instead of downloading")
include(FetchContent)
if(MYSTOCKS_GOTDX_SOURCE_DIR)
    set(gotdx_SOURCE_DIR "${MYSTOCKS_GOTDX_SOURCE_DIR}")
else()
    FetchContent_Declare(gotdx
        GIT_REPOSITORY https://github.com/bensema/gotdx.git
        GIT_TAG "${MYSTOCKS_GOTDX_REF}"
        GIT_SHALLOW FALSE
    )
    FetchContent_MakeAvailable(gotdx)
endif()
set(_tdx_bridge "${CMAKE_BINARY_DIR}/gotdx-bridge")
file(MAKE_DIRECTORY "${_tdx_bridge}")
configure_file("${CMAKE_SOURCE_DIR}/bridge/gotdx/go.mod" "${_tdx_bridge}/go.mod" COPYONLY)
file(APPEND "${_tdx_bridge}/go.mod" "\nreplace github.com/bensema/gotdx => \"${gotdx_SOURCE_DIR}\"\n")
file(GLOB _tdx_bridge_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/bridge/gotdx/*.go")
foreach(_tdx_source IN LISTS _tdx_bridge_sources)
    get_filename_component(_tdx_name "${_tdx_source}" NAME)
    configure_file("${_tdx_source}" "${_tdx_bridge}/${_tdx_name}" COPYONLY)
endforeach()
set(_tdx_calendar "${gotdx_SOURCE_DIR}/data/futures-trade-calendar.db")
# Go uses its own C compiler for cgo (MinGW GCC on Windows, clang on macOS).
# Setting CC to the MSVC compiler would break Go's c-shared build.
if(WIN32)
    set(_tdx_library "${_tdx_bridge}/mystocks_gotdx.dll")
elseif(APPLE)
    set(_tdx_library "${_tdx_bridge}/libmystocks_gotdx.dylib")
else()
    set(_tdx_library "${_tdx_bridge}/libmystocks_gotdx.so")
endif()
file(GLOB_RECURSE _tdx_sources CONFIGURE_DEPENDS "${gotdx_SOURCE_DIR}/*.go")
# Release artifacts ship this library; drop symbols and build paths there, while
# keeping them in debug builds for the Go bridge.
set(_tdx_go_release_flags "")
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_tdx_go_release_flags "-trimpath" "-ldflags=-s -w" "-buildvcs=false")
endif()
add_custom_command(OUTPUT "${_tdx_library}"
    COMMAND "${CMAKE_COMMAND}" -E env CGO_ENABLED=1 "${MYSTOCKS_GO_EXECUTABLE}" mod tidy
    COMMAND "${CMAKE_COMMAND}" -E env CGO_ENABLED=1 "${MYSTOCKS_GO_EXECUTABLE}" build -buildmode=c-shared ${_tdx_go_release_flags} -o "${_tdx_library}" .
    WORKING_DIRECTORY "${_tdx_bridge}"
    DEPENDS ${_tdx_bridge_sources} "${_tdx_bridge}/go.mod"
        "${gotdx_SOURCE_DIR}/go.mod" ${_tdx_sources}
    COMMENT "Building gotdx Go c-shared bridge"
    VERBATIM
)
if(APPLE)
    set(_tdx_destination "$<TARGET_BUNDLE_DIR:MyStocks>/Contents/Frameworks")
    # Keep non-code files out of Frameworks so codesign does not treat them as
    # nested code when signing the bundle.
    set(_tdx_data_destination "$<TARGET_BUNDLE_DIR:MyStocks>/Contents/Resources")
    install(FILES "${_tdx_library}" DESTINATION "MyStocks.app/Contents/Frameworks")
    install(FILES "${_tdx_calendar}" DESTINATION "MyStocks.app/Contents/Resources" RENAME gotdx-futures-calendar.db)
    install(FILES "${gotdx_SOURCE_DIR}/LICENSE" DESTINATION "MyStocks.app/Contents/Resources" RENAME gotdx-LICENSE.txt)
else()
    set(_tdx_destination "$<TARGET_FILE_DIR:MyStocks>")
    set(_tdx_data_destination "${_tdx_destination}")
    install(FILES "${_tdx_library}" DESTINATION bin)
    install(FILES "${_tdx_calendar}" DESTINATION bin RENAME gotdx-futures-calendar.db)
    install(FILES "${gotdx_SOURCE_DIR}/LICENSE" DESTINATION bin RENAME gotdx-LICENSE.txt)
endif()
add_custom_target(MyStocksGotdx
    DEPENDS "${_tdx_library}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_tdx_destination}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_tdx_data_destination}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_tdx_library}" "${_tdx_destination}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_tdx_calendar}" "${_tdx_data_destination}/gotdx-futures-calendar.db"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${gotdx_SOURCE_DIR}/LICENSE" "${_tdx_data_destination}/gotdx-LICENSE.txt"
    VERBATIM
)
add_dependencies(MyStocks MyStocksGotdx)
# Replacing a linked Go runtime while it is loaded is unsupported. Updating is
# explicit: fetch + reconfigure + rebuild, then restart the application.
if(NOT MYSTOCKS_GOTDX_SOURCE_DIR)
    add_custom_target(update-gotdx
        COMMAND "${CMAKE_COMMAND}" -DREPOSITORY=${gotdx_SOURCE_DIR} -DREF=${MYSTOCKS_GOTDX_REF}
            -P "${CMAKE_SOURCE_DIR}/cmake/UpdateGotdx.cmake"
        COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_SOURCE_DIR}" -B "${CMAKE_BINARY_DIR}"
        COMMENT "Updating gotdx; build MyStocks after this target completes"
        VERBATIM
    )
endif()

if(BUILD_TESTING)
    add_test(NAME gotdx_bridge COMMAND "${MYSTOCKS_GO_EXECUTABLE}" test ./...)
    set_tests_properties(gotdx_bridge PROPERTIES WORKING_DIRECTORY "${_tdx_bridge}")
endif()
