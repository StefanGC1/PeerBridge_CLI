message(STATUS "Running copy_deps.cmake at ${CMAKE_CURRENT_SOURCE_DIR}")

if(NOT DEFINED TARGET_EXE)
    message(FATAL_ERROR "TARGET_EXE not defined")
endif()

message(STATUS "Analyzing: ${TARGET_EXE}")

execute_process(
    COMMAND ntldd -R "${TARGET_EXE}"
    OUTPUT_VARIABLE LDD_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

message(STATUS "Found DLLs: ${DLL_PATHS}")

# Match all paths to .dlls
string(REGEX MATCHALL "[A-Za-z]:[^ \r\n]+\\.dll" DLL_PATHS "${LDD_OUTPUT}")

# Create release dir
file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/release")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/release")

# Copy the executable
file(COPY "${TARGET_EXE}" DESTINATION "${CMAKE_BINARY_DIR}/release")

# Copy only non-system DLLs
foreach(DLL_PATH ${DLL_PATHS})
    string(TOLOWER "${DLL_PATH}" DLL_PATH_LOWER_RAW)
    string(REPLACE "\\" "/" DLL_PATH_LOWER "${DLL_PATH_LOWER_RAW}")
    if(DLL_PATH_LOWER MATCHES ".*msys64/mingw.*/bin/.*\\.dll$")
        message(STATUS "Copying dependency: ${DLL_PATH}")
        file(COPY "${DLL_PATH}" DESTINATION "${CMAKE_BINARY_DIR}/release")
    else()
        message(STATUS "Skipping system DLL: ${DLL_PATH}")
    endif()
endforeach()

# Copy the Wintun driver DLL to release dir
file(COPY "${CMAKE_BINARY_DIR}/wintun.dll" DESTINATION "${CMAKE_BINARY_DIR}/release")

# (Optional) Copy readme/config files
file(GLOB EXTRA_FILES "${CMAKE_SOURCE_DIR}/README.*")
file(COPY ${EXTRA_FILES} DESTINATION "${CMAKE_BINARY_DIR}/release")
