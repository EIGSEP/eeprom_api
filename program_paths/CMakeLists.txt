# Build the RF-switch path programmer.
#
# Drop this folder into the eeprom_api repo (alongside test/) and add
#   add_subdirectory(program_paths)
# to the repo's top-level CMakeLists.txt. Then, from build/:
#   cmake .. -DPICO_BOARD=pico2
#   make program_paths
# Output: build/program_paths/program_paths.uf2
#
# The 'at28bv64b' target is defined by the parent CMakeLists.

add_executable(program_paths program_paths.c)
target_link_libraries(program_paths at28bv64b pico_stdlib)
pico_add_extra_outputs(program_paths)          # generates .uf2
pico_enable_stdio_usb(program_paths 1)         # prompts/output over USB CDC
pico_enable_stdio_uart(program_paths 0)

# Read-back / decode tester. Build with: make test_paths
# Output: build/program_paths/test_paths.uf2
add_executable(test_paths test_paths.c)
target_link_libraries(test_paths at28bv64b pico_stdlib)
pico_add_extra_outputs(test_paths)
pico_enable_stdio_usb(test_paths 1)
pico_enable_stdio_uart(test_paths 0)

# Runtime path selector. Drives A0..A4 on GP8..GP12; no EEPROM library needed.
# Build with: make select_path   ->  build/program_paths/select_path.uf2
add_executable(select_path select_path.c)
target_link_libraries(select_path pico_stdlib)
pico_add_extra_outputs(select_path)
pico_enable_stdio_usb(select_path 1)
pico_enable_stdio_uart(select_path 0)
