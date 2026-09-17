idf_component_register(
        SRCS "owb.c" "owb_gpio.c" "owb_rmt.c"
        REQUIRES driver
        INCLUDE_DIRS "include")


