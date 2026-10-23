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

# Portable firmware logic: no OS, no heap, no I/O. This is what would be built for the Cortex-M3.
add_library(ws_core STATIC
    src/ws_crc.c
    src/ws_protocol.c
)
target_include_directories(ws_core PUBLIC include)

enable_testing()

add_executable(test_protocol test/test_protocol.c)
target_link_libraries(test_protocol PRIVATE ws_core)
add_test(NAME protocol COMMAND test_protocol)

# Shared test vectors for every decoder. Regenerate with:
#   ./build/gen_vectors > protocol/vectors-v1.json
add_executable(gen_vectors tools/gen_vectors.c)
target_include_directories(gen_vectors PRIVATE test)
target_link_libraries(gen_vectors PRIVATE ws_core)
add_test(NAME vectors_up_to_date
         COMMAND sh -c "$<TARGET_FILE:gen_vectors> | diff -u ${CMAKE_SOURCE_DIR}/../protocol/vectors-v1.json -")
