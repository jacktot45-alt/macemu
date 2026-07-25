cmake_minimum_required(VERSION 3.16)
project(macemu LANGUAGES CXX C VERSION 0.4.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
  add_compile_options(/W3)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# Optional SDL2 (Fase 4 window/rendering backend).
# Without SDL2 the project still builds; the "null" backend is used instead and
# frames can be dumped to .ppm files for inspection.
# ---------------------------------------------------------------------------
option(MACEMU_USE_SDL2 "Build the SDL2 window backend if SDL2 is available" ON)

set(MACEMU_HAVE_SDL2 OFF)
if(MACEMU_USE_SDL2)
  find_package(SDL2 QUIET)
  if(SDL2_FOUND)
    set(MACEMU_HAVE_SDL2 ON)
  else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(SDL2PC QUIET sdl2)
      if(SDL2PC_FOUND)
        set(MACEMU_HAVE_SDL2 ON)
      endif()
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(macemu_core STATIC
  src/util.cpp
  src/memory.cpp
  src/pe/pe.cpp
  src/pe/feasibility.cpp
  src/pe/loader.cpp
  src/cpu/decoder.cpp
  src/cpu/exec.cpp
  src/cpu/exec_sse.cpp
  src/cpu/exec_x87.cpp
  src/cpu/disasm.cpp
  src/win32/win32.cpp
  src/win32/kernel32.cpp
  src/win32/user32.cpp
  src/win32/gdi32.cpp
  src/win32/misc.cpp
  src/host/framebuffer.cpp
  src/host/window.cpp
  src/emulator.cpp
)

target_include_directories(macemu_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MACEMU_HAVE_SDL2)
  target_compile_definitions(macemu_core PUBLIC MACEMU_HAVE_SDL2=1)
  if(SDL2_FOUND)
    if(TARGET SDL2::SDL2)
      target_link_libraries(macemu_core PUBLIC SDL2::SDL2)
    else()
      target_include_directories(macemu_core PUBLIC ${SDL2_INCLUDE_DIRS})
      target_link_libraries(macemu_core PUBLIC ${SDL2_LIBRARIES})
    endif()
  else()
    target_include_directories(macemu_core PUBLIC ${SDL2PC_INCLUDE_DIRS})
    target_link_libraries(macemu_core PUBLIC ${SDL2PC_LIBRARIES})
    target_link_directories(macemu_core PUBLIC ${SDL2PC_LIBRARY_DIRS})
  endif()
  message(STATUS "macemu: SDL2 backend ENABLED")
else()
  message(STATUS "macemu: SDL2 backend DISABLED (headless/null backend only)")
endif()

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(macemu src/main.cpp)
target_link_libraries(macemu PRIVATE macemu_core)

add_executable(macemu-peinfo tools/peinfo_main.cpp)
target_link_libraries(macemu-peinfo PRIVATE macemu_core)

# ---------------------------------------------------------------------------
# Test-fixtures: kleine .exe's die met tools/mkpe.py worden gegenereerd.
# Ze staan ook in de repo, zodat de tests werken zonder Python.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_custom_target(fixtures ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/mkpe.py
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Test-executables genereren met tools/mkpe.py"
    VERBATIM)
else()
  message(STATUS "macemu: geen Python3 gevonden - fixtures/ uit de repo worden gebruikt")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(test_cpu tests/test_cpu.cpp)
target_link_libraries(test_cpu PRIVATE macemu_core)
add_test(NAME cpu COMMAND test_cpu)

add_executable(test_memory tests/test_memory.cpp)
target_link_libraries(test_memory PRIVATE macemu_core)
add_test(NAME memory COMMAND test_memory)

add_executable(test_pe tests/test_pe.cpp)
target_link_libraries(test_pe PRIVATE macemu_core)
add_test(NAME pe COMMAND test_pe WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_integration tests/test_integration.cpp)
target_link_libraries(test_integration PRIVATE macemu_core)
add_test(NAME integration COMMAND test_integration WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS macemu macemu-peinfo RUNTIME DESTINATION bin)
