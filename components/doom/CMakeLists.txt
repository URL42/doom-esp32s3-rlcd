# Vendored Doom source plus the RLCD platform layer.
#
# GLOB rather than an explicit list: this is vendored third-party source we do
# not add files to casually, and an 80-file list would be noise. The tradeoff is
# that CMake must be re-run when files are added -- `idf.py fullclean` if a new
# source file is not picked up.
file(GLOB DOOM_SOURCES    ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
file(GLOB PLATFORM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/platform/*.c)

idf_component_register(
    SRCS ${DOOM_SOURCES} ${PLATFORM_SOURCES}
    INCLUDE_DIRS "include" "platform"
    REQUIRES esp_partition esp_timer esp_system freertos heap log vfs
             esp_driver_usb_serial_jtag esp_driver_spi esp_driver_gpio
)

# 1993 C, compiled by GCC 14. None of these indicate a real defect in the game;
# they are the cost of building code written before most of these warnings
# existed. Scoped PRIVATE so they do not leak into other components.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-char-subscripts
    -Wno-unused-value
    -Wno-parentheses
    -Wno-int-to-pointer-cast     # ESP_DOOM had this misspelled "-cas", so it silently did nothing
    -Wno-pointer-sign
    -Wno-unused-but-set-parameter
    -Wno-unused-but-set-variable
    -Wno-maybe-uninitialized
    -Wno-unused-variable
    -Wno-unused-const-variable
    -Wno-format-truncation
    -Wno-implicit-fallthrough
    -Wno-misleading-indentation

    # R_SortVisSprites builds a circular doubly-linked list around a stack-local
    # sentinel node and unlinks every entry before returning, so the address
    # never outlives the frame. GCC 14 cannot prove that and flags it.
    -Wno-dangling-pointer
)
