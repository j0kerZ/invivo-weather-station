cmake_minimum_required(VERSION 3.20)
project(weather_station_firmware C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Warnings are errors: on a device that runs unattended for years, a conversion warning is a bug.
add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)

# Host builds run the tests under AddressSanitizer and UndefinedBehaviorSanitizer, which catch
# out-of-bounds access and undefined integer behaviour that the MCU would silently tolerate.
option(WS_SANITIZE "Build with ASan and UBSan" ON)
if(WS_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Portable firmware: no OS, no heap, no floating point. This is what would be built for the
# Cortex-M3. Hardware access goes through ws_hal.h, implemented by the board or the emulator.
add_library(ws_core STATIC
    src/ws_crc.c
    src/ws_protocol.c
    src/ws_lm92.c
    src/ws_hdc3020.c
    src/ws_aggregator.c
    src/ws_station.c
    src/ws_store.c
    src/ws_cursor.c
    src/ws_uplink.c
)
target_include_directories(ws_core PUBLIC include)

# coreMQTT: AWS's MQTT client for microcontrollers, pinned to a release. Third-party code (SOUP)
# is used as released, so our warning flags are not applied to it.
include(FetchContent)
FetchContent_Declare(coremqtt
    GIT_REPOSITORY https://github.com/FreeRTOS/coreMQTT.git
    GIT_TAG v5.0.2
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR cmake-not-used) # fetch only: its own targets would spread its sources to ours
FetchContent_MakeAvailable(coremqtt)
include(${coremqtt_SOURCE_DIR}/mqttFilePaths.cmake)
add_library(coremqtt STATIC ${MQTT_SOURCES} ${MQTT_SERIALIZER_SOURCES})
target_include_directories(coremqtt SYSTEM PUBLIC ${MQTT_INCLUDE_PUBLIC_DIRS})
target_compile_definitions(coremqtt PUBLIC MQTT_DO_NOT_USE_CUSTOM_CONFIG)
target_compile_options(coremqtt PRIVATE -w)

# Portable MQTT adapter (would also run on the station).
add_library(ws_mqtt STATIC src/ws_mqtt.c)
target_link_libraries(ws_mqtt PUBLIC ws_core coremqtt)

# PC emulation of the hardware (sensors on I2C, the weather), and the command-line emulator.
add_library(ws_emu STATIC emu/emu.c emu/emu_flash.c)
target_include_directories(ws_emu PUBLIC emu)
target_link_libraries(ws_emu PUBLIC ws_core m)

# PC network transport for coreMQTT: TCP + TLS via OpenSSL.
find_package(OpenSSL REQUIRED)
add_library(ws_emu_net STATIC emu/emu_net.c)
target_include_directories(ws_emu_net PUBLIC emu)
target_link_libraries(ws_emu_net PUBLIC coremqtt OpenSSL::SSL)

add_executable(ws-emulator emu/main.c emu/online.c)
target_link_libraries(ws-emulator PRIVATE ws_emu ws_mqtt ws_emu_net)

enable_testing()

add_executable(test_protocol test/test_protocol.c)
target_link_libraries(test_protocol PRIVATE ws_core)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_sensors test/test_sensors.c) # provides its own fake HAL
target_link_libraries(test_sensors PRIVATE ws_core)
add_test(NAME sensors COMMAND test_sensors)

add_executable(test_aggregator test/test_aggregator.c)
target_link_libraries(test_aggregator PRIVATE ws_core)
add_test(NAME aggregator COMMAND test_aggregator)

add_executable(test_emulator test/test_emulator.c)
target_link_libraries(test_emulator PRIVATE ws_emu)
add_test(NAME emulator COMMAND test_emulator)

add_executable(test_store test/test_store.c)
target_link_libraries(test_store PRIVATE ws_emu)
add_test(NAME store COMMAND test_store)

add_executable(test_cursor test/test_cursor.c)
target_link_libraries(test_cursor PRIVATE ws_emu)
add_test(NAME cursor COMMAND test_cursor)

add_executable(test_uplink test/test_uplink.c)
target_link_libraries(test_uplink PRIVATE ws_emu)
add_test(NAME uplink COMMAND test_uplink)

add_executable(test_mqtt test/test_mqtt.c)
target_link_libraries(test_mqtt PRIVATE ws_mqtt)
add_test(NAME mqtt COMMAND test_mqtt)

# The Cortex-M3 has no FPU: floating point in firmware code would pull in slow software routines.
add_test(NAME no_floating_point_in_firmware
         COMMAND sh -c "! grep -nwE 'float|double' ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/include/*.h")

# Shared test vectors for every decoder. Regenerate with:
#   ./build/gen_vectors > protocol/vectors-v1.json
add_executable(gen_vectors tools/gen_vectors.c)
target_include_directories(gen_vectors PRIVATE test)
target_link_libraries(gen_vectors PRIVATE ws_core)
add_test(NAME vectors_up_to_date
         COMMAND sh -c "$<TARGET_FILE:gen_vectors> | diff -u ${CMAKE_SOURCE_DIR}/../protocol/vectors-v1.json -")
