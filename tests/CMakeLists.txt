cmake_minimum_required(VERSION 3.16)
project(esphome_weber_connect_protocol_tests LANGUAGES CXX)

add_executable(
  protocol_vectors
  protocol_vectors.cpp
  ../components/weber_connect_ble/crc8.cpp
  ../components/weber_connect_ble/pairing.cpp
  ../components/weber_connect_ble/saber_parser.cpp
)
target_include_directories(protocol_vectors PRIVATE ../components/weber_connect_ble)
target_compile_features(protocol_vectors PRIVATE cxx_std_17)
target_compile_options(protocol_vectors PRIVATE -Wall -Wextra -Werror)
