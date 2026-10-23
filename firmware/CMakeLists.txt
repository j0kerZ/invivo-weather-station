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
)
target_include_directories(ws_core PUBLIC include)

# PC emulation of the hardware (sensors on I2C, the weather), and the command-line emulator.
add_library(ws_emu STATIC emu/emu.c)
target_include_directories(ws_emu PUBLIC emu)
target_link_libraries(ws_emu PUBLIC ws_core m)

add_executable(ws-emulator emu/main.c)
target_link_libraries(ws-emulator PRIVATE ws_emu)

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
