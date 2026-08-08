cmake_minimum_required(VERSION 3.24)

project(maimai_controller_H503 LANGUAGES C ASM)

set(FIRMWARE_NAME maimai_controller_H503)
set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/STM32H503CBTX_FLASH.ld")

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Firmware build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)

# CubeIDE defines the complete Debug/Release flag sets itself. Clear CMake's
# desktop-oriented defaults (for example -O3 and -DNDEBUG in Release) so the
# portable build stays aligned with the CubeIDE configurations.
foreach(LANGUAGE C ASM)
    set(CMAKE_${LANGUAGE}_FLAGS_DEBUG "" CACHE STRING "" FORCE)
    set(CMAKE_${LANGUAGE}_FLAGS_RELEASE "" CACHE STRING "" FORCE)
endforeach()

file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/App/*.c"
)
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/*.c"
)
file(GLOB HAL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32H5xx_HAL_Driver/Src/*.c"
)

set(TINYUSB_SOURCES
    tinyusb/tusb.c
    tinyusb/class/cdc/cdc_device.c
    tinyusb/class/hid/hid_device.c
    tinyusb/common/tusb_fifo.c
    tinyusb/device/usbd.c
    tinyusb/device/usbd_control.c
    tinyusb/portable/st/stm32_fsdev/dcd_stm32_fsdev.c
)

add_executable(${FIRMWARE_NAME}
    ${APP_SOURCES}
    ${CORE_SOURCES}
    ${HAL_SOURCES}
    ${TINYUSB_SOURCES}
    Core/Startup/startup_stm32h503cbtx.s
)

set_target_properties(${FIRMWARE_NAME} PROPERTIES
    OUTPUT_NAME "${FIRMWARE_NAME}"
    SUFFIX ".elf"
)

target_include_directories(${FIRMWARE_NAME} PRIVATE
    App
    Core/Inc
    Drivers/STM32H5xx_HAL_Driver/Inc
    Drivers/STM32H5xx_HAL_Driver/Inc/Legacy
    Drivers/CMSIS/Device/ST/STM32H5xx/Include
    Drivers/CMSIS/Include
    tinyusb
)

target_compile_definitions(${FIRMWARE_NAME} PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    USE_HAL_DRIVER
    STM32H503xx
)

target_compile_options(${FIRMWARE_NAME} PRIVATE
    -mcpu=cortex-m33
    -mthumb
    -mfpu=fpv5-sp-d16
    -mfloat-abi=hard
    -ffunction-sections
    -fdata-sections
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    $<$<AND:$<COMPILE_LANGUAGE:C>,$<CONFIG:Debug>>:-O0>
    $<$<AND:$<COMPILE_LANGUAGE:C>,$<CONFIG:Debug>>:-g3>
    $<$<AND:$<COMPILE_LANGUAGE:C>,$<CONFIG:Release>>:-Os>
    $<$<AND:$<COMPILE_LANGUAGE:C>,$<CONFIG:Release>>:-g0>
    $<$<COMPILE_LANGUAGE:C>:-Wall>
    $<$<COMPILE_LANGUAGE:C>:-fstack-usage>
    $<$<COMPILE_LANGUAGE:C>:-fcyclomatic-complexity>
    $<$<COMPILE_LANGUAGE:C>:--specs=nano.specs>
    $<$<COMPILE_LANGUAGE:ASM>:-x>
    $<$<COMPILE_LANGUAGE:ASM>:assembler-with-cpp>
    $<$<AND:$<COMPILE_LANGUAGE:ASM>,$<CONFIG:Debug>>:-g3>
    $<$<AND:$<COMPILE_LANGUAGE:ASM>,$<CONFIG:Release>>:-g0>
)

target_link_options(${FIRMWARE_NAME} PRIVATE
    -mcpu=cortex-m33
    -mthumb
    -mfpu=fpv5-sp-d16
    -mfloat-abi=hard
    "-T${LINKER_SCRIPT}"
    --specs=nosys.specs
    --specs=nano.specs
    -Wl,--gc-sections
    -static
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${FIRMWARE_NAME}.map"
    -Wl,--start-group
    -lc
    -lm
    -Wl,--end-group
)

add_custom_command(TARGET ${FIRMWARE_NAME} POST_BUILD
    COMMAND "${CMAKE_OBJCOPY}" -O ihex
            "$<TARGET_FILE:${FIRMWARE_NAME}>"
            "${CMAKE_CURRENT_BINARY_DIR}/${FIRMWARE_NAME}.hex"
    COMMAND "${CMAKE_OBJCOPY}" -O binary
            "$<TARGET_FILE:${FIRMWARE_NAME}>"
            "${CMAKE_CURRENT_BINARY_DIR}/${FIRMWARE_NAME}.bin"
    COMMAND "${CMAKE_SIZE}" "$<TARGET_FILE:${FIRMWARE_NAME}>"
    BYPRODUCTS
        "${CMAKE_CURRENT_BINARY_DIR}/${FIRMWARE_NAME}.hex"
        "${CMAKE_CURRENT_BINARY_DIR}/${FIRMWARE_NAME}.bin"
        "${CMAKE_CURRENT_BINARY_DIR}/${FIRMWARE_NAME}.map"
    COMMENT "Generating firmware HEX/BIN images and size report"
)
