idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES prboom prboom-esp32-compat esp_partition
)
