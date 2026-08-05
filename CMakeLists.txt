cmake_minimum_required(VERSION 3.16)

project(swiftlink
  VERSION 0.1.0
  DESCRIPTION "Reliable file transfer over UDP"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++20, not -std=gnu++20
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd / editor tooling

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Shared warning flags, attached to every target below rather than set globally,
# so they never leak into anything the project might pull in later.
add_library(swiftlink_warnings INTERFACE)
target_compile_options(swiftlink_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)

# ---------------------------------------------------------------------------
# Protocol: packet format and (de)serialisation. No sockets, no I/O, no OS
# dependency -- which is what makes it directly unit-testable.
# ---------------------------------------------------------------------------
add_library(swiftlink_protocol STATIC
  src/protocol/packet.cpp
  src/protocol/crc32.cpp
  src/protocol/status.cpp
  src/protocol/serializer.cpp)

target_include_directories(swiftlink_protocol PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(swiftlink_protocol PRIVATE swiftlink_warnings)

# ---------------------------------------------------------------------------
# Net: the only code that touches file descriptors. Depends on the protocol
# library for nothing at all -- it moves opaque bytes.
# ---------------------------------------------------------------------------
add_library(swiftlink_net STATIC
  src/net/udp_socket.cpp)

target_include_directories(swiftlink_net PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(swiftlink_net PRIVATE swiftlink_warnings)

# ---------------------------------------------------------------------------
# Crypto: SHA-256 from scratch, for end-to-end file verification.
# ---------------------------------------------------------------------------
add_library(swiftlink_crypto STATIC
  src/crypto/sha256.cpp)

target_include_directories(swiftlink_crypto PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(swiftlink_crypto PRIVATE swiftlink_warnings)

# ---------------------------------------------------------------------------
# IO: RAII file descriptor with positional (pread/pwrite) access.
# ---------------------------------------------------------------------------
add_library(swiftlink_io STATIC
  src/io/file.cpp)

target_include_directories(swiftlink_io PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(swiftlink_io PRIVATE swiftlink_warnings)

# ---------------------------------------------------------------------------
# Transfer: the reliability layer. Sits above protocol, net and io, and is the
# first layer that knows what a "file transfer" is.
# ---------------------------------------------------------------------------
add_library(swiftlink_transfer STATIC
  src/transfer/transfer.cpp
  src/transfer/filename.cpp
  src/transfer/window.cpp
  src/transfer/sender.cpp
  src/transfer/receiver_session.cpp
  src/transfer/receiver.cpp)

target_include_directories(swiftlink_transfer PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(swiftlink_transfer
  PUBLIC swiftlink_protocol swiftlink_net swiftlink_io swiftlink_crypto
  PRIVATE swiftlink_warnings)

# ---------------------------------------------------------------------------
# Executables: the only place the layers are wired together.
# ---------------------------------------------------------------------------
add_executable(swiftlink_server
  src/server/server_main.cpp
  src/server/epoll_server.cpp)
target_link_libraries(swiftlink_server
  PRIVATE swiftlink_transfer swiftlink_warnings)

add_executable(swiftlink_client src/client/client_main.cpp)
target_link_libraries(swiftlink_client
  PRIVATE swiftlink_transfer swiftlink_warnings)

# ---------------------------------------------------------------------------
# Tests. Hand-rolled harness -- the project takes no external dependencies, so
# no GoogleTest. `ctest` runs it; a non-zero exit is a failure.
# ---------------------------------------------------------------------------
enable_testing()

add_executable(protocol_tests tests/protocol_tests.cpp)
target_link_libraries(protocol_tests
  PRIVATE swiftlink_protocol swiftlink_warnings)

add_test(NAME protocol_tests COMMAND protocol_tests)

# ---------------------------------------------------------------------------
# Benchmark tooling. Not part of the transfer system: delay_proxy exists only
# because this development kernel is built without netem (see the comment at
# the top of tools/delay_proxy.cpp).
# ---------------------------------------------------------------------------
add_executable(delay_proxy tools/delay_proxy.cpp)
target_link_libraries(delay_proxy PRIVATE swiftlink_warnings)

add_executable(crypto_tests tests/crypto_tests.cpp)
target_link_libraries(crypto_tests
  PRIVATE swiftlink_crypto swiftlink_protocol swiftlink_transfer swiftlink_warnings)

add_test(NAME crypto_tests COMMAND crypto_tests)
