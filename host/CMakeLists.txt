cmake_minimum_required(VERSION 3.20)
project(RemoteHost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# libdatachannel ставится через vcpkg (см. инструкцию), тулчейн задаётся в CMakeSettings.json
find_package(LibDataChannel CONFIG REQUIRED)

add_executable(host
  main.cpp
  capture.cpp
  convert.cpp
  encoder.cpp
  webrtc.cpp
)

target_link_libraries(host PRIVATE
  LibDataChannel::LibDataChannel
  d3d11 dxgi
  mfplat mf mfuuid strmiids
  ole32 oleaut32
)
target_compile_definitions(host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
