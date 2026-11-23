if(CONFIG_BRIDGE_LUATOS)
    set(bridge_sources "luatos_main.c" "luatos_frame.c")
else()
    set(bridge_sources "bridge_main.c" "bridge_control.c")
endif()

idf_component_register(
    SRCS ${bridge_sources}
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_uart esp_driver_usb_serial_jtag esp_timer
)
