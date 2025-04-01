cmake_minimum_required(VERSION 3.10)
project(P2PChat)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS system filesystem thread)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSODIUM REQUIRED libsodium)
pkg_check_modules(OPENSSL REQUIRED openssl)
pkg_check_modules(ZLIB REQUIRED zlib)

set(NLOHMANN_INCLUDEDIR "${CMAKE_SOURCE_DIR}/external/nlohmann/include")

set(IXWS_ROOT "${CMAKE_SOURCE_DIR}/external/ixwebsocket")
set(IXWS_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/external/ixwebsocket/include")
set(IXWS_LIBRARY "${CMAKE_SOURCE_DIR}/external/ixwebsocket/lib/libixwebsocket.a")

include_directories(${IXWS_INCLUDE_DIRS})
link_directories(${IXWS_ROOT}/lib)


# Project source and header files
include_directories("${CMAKE_SOURCE_DIR}/include")
set(SOURCES
    src/main.cpp
    src/crypto.cpp
    src/networking.cpp
    src/signaling.cpp
    src/stun.cpp
)

# Create executable
add_executable(P2PChat ${SOURCES})

# Include directories
target_include_directories(P2PChat PRIVATE
    ${Boost_INCLUDE_DIRS}
    ${LIBSODIUM_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
    ${NLOHMANN_INCLUDEDIR}
)

# Link libraries
target_link_libraries(P2PChat PRIVATE
    ${Boost_LIBRARIES}
    ${LIBSODIUM_LIBRARIES}
    ${IXWS_LIBRARY}
    ${OPENSSL_LIBRARIES}
    ${ZLIB_LIBRARIES}
    ws2_32
    crypt32
)

target_compile_definitions(P2PChat PRIVATE IXWEBSOCKET_USE_TLS)

#### POST-BUILD PACKAGING ####

option(ENABLE_DEP_COPY "Copy DLL dependencies to release folder and generate ZIP" OFF)

if(ENABLE_DEP_COPY)
    # Copy dependencies automatically
    add_custom_target(copy_deps ALL
        COMMAND ${CMAKE_COMMAND}
            -DTARGET_EXE=$<TARGET_FILE:P2PChat>
            -P "${CMAKE_SOURCE_DIR}/copy_deps.cmake"
        BYPRODUCTS "${CMAKE_BINARY_DIR}/release/P2PChat.exe"
        VERBATIM
    )
    add_dependencies(copy_deps P2PChat)

    # Install step: include the release folder contents in the package
    install(DIRECTORY "${CMAKE_BINARY_DIR}/release/" DESTINATION .)

    # Package config (CPack ZIP)
    include(InstallRequiredSystemLibraries)
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_NAME "P2PChat")
    set(CPACK_PACKAGE_VERSION "1.0.0")
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
    set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/package")
    include(CPack)
endif()