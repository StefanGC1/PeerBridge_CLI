cmake_minimum_required(VERSION 3.10)
project(P2PNet)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS system filesystem thread stacktrace_windbg)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSODIUM REQUIRED libsodium)
pkg_check_modules(OPENSSL REQUIRED openssl)
pkg_check_modules(ZLIB REQUIRED zlib)
pkg_check_modules(QUILL REQUIRED quill)

set(NLOHMANN_INCLUDEDIR "${CMAKE_SOURCE_DIR}/external/nlohmann/include")

set(IXWS_ROOT "${CMAKE_SOURCE_DIR}/external/ixwebsocket")
set(IXWS_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/external/ixwebsocket/include")
set(IXWS_LIBRARY "${CMAKE_SOURCE_DIR}/external/ixwebsocket/lib/libixwebsocket.a")

set(WINTUN_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/wintun/include")
message(STATUS "wintun: ${WINTUN_INCLUDE_DIR}")
set(WINTUN_DLL_DIR "${CMAKE_SOURCE_DIR}/wintun/bin")

include_directories(${IXWS_INCLUDE_DIRS})
include_directories(${WINTUN_INCLUDE_DIR})
link_directories(${IXWS_ROOT}/lib)


# Project source and header files
include_directories("${CMAKE_SOURCE_DIR}/include")
set(SOURCES
    src/main.cpp
    src/crypto.cpp
    src/networking.cpp
    src/signaling.cpp
    src/stun.cpp
    src/p2p_system.cpp
    src/tun_interface.cpp
    src/logger.cpp
    src/networkconfigmanager.cpp
    src/systemstatemanager.cpp
)

# Create executable
add_executable(P2PNet ${SOURCES})

# Include directories
target_include_directories(P2PNet PRIVATE
    ${Boost_INCLUDE_DIRS}
    ${LIBSODIUM_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
    ${NLOHMANN_INCLUDEDIR}
    ${QUILL_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(P2PNet PRIVATE
    ${Boost_LIBRARIES}
    Boost::stacktrace_windbg
    dbghelp
    ${LIBSODIUM_LIBRARIES}
    ${IXWS_LIBRARY}
    ${OPENSSL_LIBRARIES}
    ${ZLIB_LIBRARIES}
    ws2_32
    mswsock
    crypt32
    iphlpapi
)

# Copy Wintun driver dll to build dir
add_custom_command(TARGET P2PNet POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WINTUN_DLL_DIR}/wintun.dll"
            $<TARGET_FILE_DIR:P2PNet>
    )

target_compile_definitions(P2PNet PRIVATE IXWEBSOCKET_USE_TLS)
target_compile_definitions(P2PNet PRIVATE SOURCE_ROOT_DIR="${CMAKE_SOURCE_DIR}/src/")

#### POST-BUILD PACKAGING ####

option(ENABLE_DEP_COPY "Copy DLL dependencies to release folder and generate ZIP" OFF)

if(ENABLE_DEP_COPY)
    # Copy dependencies automatically
    add_custom_target(copy_deps ALL
        COMMAND ${CMAKE_COMMAND}
            -DTARGET_EXE=$<TARGET_FILE:P2PNet>
            -P "${CMAKE_SOURCE_DIR}/copy_deps.cmake"
        BYPRODUCTS "${CMAKE_BINARY_DIR}/release/P2PNet.exe"
        VERBATIM
    )
    add_dependencies(copy_deps P2PNet)

    # Install step: include the release folder contents in the package
    install(DIRECTORY "${CMAKE_BINARY_DIR}/release/" DESTINATION .)

    # Package config (CPack ZIP)
    include(InstallRequiredSystemLibraries)
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_NAME "P2PNet")
    set(CPACK_PACKAGE_VERSION "1.0.0")
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
    set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package")
    include(CPack)
endif()