cmake_minimum_required(VERSION 3.22)
project(mango_native C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Portable core: decoder + interpreter + ELF32 parsing, no Android/NDK
# headers. Builds and tests on any host, that's on purpose, see
# native/README.md.
add_library(mango_core STATIC
  src/decoder.c
  src/interp.c
  src/elf32.c
)
target_include_directories(mango_core PUBLIC include)
set_target_properties(mango_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

enable_testing()
add_executable(mango_core_tests tests/test_interp.c)
target_link_libraries(mango_core_tests PRIVATE mango_core m)
add_test(NAME interp_smoke_test COMMAND mango_core_tests)

add_executable(mango_elf32_tests tests/test_elf32.c)
target_link_libraries(mango_elf32_tests PRIVATE mango_core)
add_test(NAME elf32_smoke_test COMMAND mango_elf32_tests)

# native_bridge_shim.c against a minimal, clearly-fake jni.h (see
# tests/fake_jni/jni.h), not the real NDK one: this is a host build,
# specifically to exercise the isCompatibleWith/v3 logic without needing
# a real device. The real Android .so below never sees this fake header.
add_executable(mango_native_bridge_shim_tests
  src/native_bridge_shim.c
  tests/test_native_bridge_shim.c
)
target_include_directories(mango_native_bridge_shim_tests PRIVATE tests/fake_jni)
target_link_libraries(mango_native_bridge_shim_tests PRIVATE mango_core m)
add_test(NAME native_bridge_shim_smoke_test COMMAND mango_native_bridge_shim_tests)

# The actual native bridge .so. Only buildable with the NDK toolchain file,
# see docs/BUILDING.md. ANDROID is set by android.toolchain.cmake.
if(ANDROID)
  add_library(mango_translator SHARED
    src/native_bridge_shim.c
  )
  target_link_libraries(mango_translator PRIVATE mango_core m log)
  target_include_directories(mango_translator PRIVATE include)
endif()

# Host-only Unity/OFDP drive helper for Test Lab (not a ctest). Loads real
# ARM32 libmain.so siblings through the same shim as mango_native_bridge_shim_tests.
add_executable(mango_host_unity_drive
  src/native_bridge_shim.c
  tests/host_unity_drive.c
)
target_include_directories(mango_host_unity_drive PRIVATE tests/fake_jni)
target_link_libraries(mango_host_unity_drive PRIVATE mango_core m)

# Host-only Meritous/SDL drive helper for Test Lab (not a ctest). Loads
# armeabi libsdl-1.2.so + libapplication.so through the same shim.
add_executable(mango_host_meritous_drive
  src/native_bridge_shim.c
  tests/host_meritous_drive.c
)
target_include_directories(mango_host_meritous_drive PRIVATE tests/fake_jni)
target_link_libraries(mango_host_meritous_drive PRIVATE mango_core m)
