idf_component_register(
    SRCS
        "gamepad.c"
        "i_main.c"
        "i_network.c"
        "i_sound.c"
        "i_system.c"
        "i_video.c"
        "psxcontroller.c"
        "spi_lcd.c"
        "tab5_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES prboom esp_driver_gpio esp_driver_i2c esp_driver_spi freertos spi_flash esp_partition
    PRIV_REQUIRES esp_psram spiffs
)

if(CONFIG_HW_M5STACK_TAB5)
    idf_component_optional_requires(PUBLIC m5stack_tab5)
    idf_component_optional_requires(PRIVATE esp_driver_ppa esp_timer)
endif()

# Explicit developer-only hardware regression. Never included in normal builds.
option(TAB5_SAVE_SELFTEST "Run destructive slot-8 save/reboot/load regression" OFF)
if(TAB5_SAVE_SELFTEST)
    target_sources(${COMPONENT_LIB} PRIVATE "../../tests/save_selftest.c")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC TAB5_SAVE_SELFTEST=1)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=char-subscripts
    -Wno-error=unused-value
    -Wno-error=parentheses
    -Wno-error=int-to-pointer-cast
    -Wno-pointer-sign
    -Wno-error=unused-but-set-parameter
    -Wno-error=maybe-uninitialized
)
