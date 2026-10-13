# CMake toolchain for the Teensy 4.1: Arm's GNU toolchain and PJRC's Teensy 4 core.
# moondeck/build/build_teensy.py passes where setup_teensy.py installed them, at the versions pinned there.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT TEENSY_SDK OR NOT TEENSY_GCC_BIN OR NOT TEENSYDUINO)
    message(FATAL_ERROR "Configure through moondeck/build/build_teensy.py, which passes TEENSY_SDK, TEENSY_GCC_BIN and TEENSYDUINO")
endif()
# The compiler checks run in sub-projects, which see these only when listed.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES TEENSY_SDK TEENSY_GCC_BIN TEENSYDUINO)

set(CMAKE_C_COMPILER   "${TEENSY_GCC_BIN}/arm-none-eabi-gcc")
set(CMAKE_CXX_COMPILER "${TEENSY_GCC_BIN}/arm-none-eabi-g++")
set(CMAKE_ASM_COMPILER "${TEENSY_GCC_BIN}/arm-none-eabi-gcc")
set(CMAKE_AR           "${TEENSY_GCC_BIN}/arm-none-eabi-gcc-ar")
set(CMAKE_RANLIB       "${TEENSY_GCC_BIN}/arm-none-eabi-gcc-ranlib")
set(CMAKE_OBJCOPY      "${TEENSY_GCC_BIN}/arm-none-eabi-objcopy" CACHE FILEPATH "")
set(CMAKE_SIZE         "${TEENSY_GCC_BIN}/arm-none-eabi-size" CACHE FILEPATH "")

# A bare-metal target cannot link a test program, so the compiler check only compiles.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# PJRC's Teensy 4.1 flags (boards.txt, teensy41), at its "Faster" optimization and USB serial.
set(TEENSY_CPU_FLAGS "-mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16")
set(TEENSY_DEFS "-D__IMXRT1062__ -DTEENSYDUINO=${TEENSYDUINO} -DARDUINO=10819 -DARDUINO_TEENSY41 -DF_CPU=600000000 -DUSB_SERIAL -DLAYOUT_US_ENGLISH")
set(CMAKE_C_FLAGS_INIT   "${TEENSY_CPU_FLAGS} ${TEENSY_DEFS} -O2 -g -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "${TEENSY_CPU_FLAGS} ${TEENSY_DEFS} -O2 -g -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -fno-threadsafe-statics -felide-constructors -Wno-psabi")
set(CMAKE_ASM_FLAGS_INIT "${TEENSY_CPU_FLAGS} ${TEENSY_DEFS} -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${TEENSY_CPU_FLAGS} -O2 -Wl,--gc-sections,--relax,--no-warn-rwx-segments")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
