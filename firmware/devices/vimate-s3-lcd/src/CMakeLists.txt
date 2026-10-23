idf_component_register(
  SRCS "main.c" "ble.c" "display_st77922.c" "display_ili9341.c" "touch_st77922.c" "touch_ft6236.c" "agents.c" "link.c" "ui.c" "debug.c" "critter.c" "hats.c" "font_vn_12.c" "font_vn_14.c" "font_vn_16.c" "font_vn_20.c" "font_vn_24.c" "font_vn_28.c"
  INCLUDE_DIRS "."
  REQUIRES deck-link driver esp_timer esp_driver_usb_serial_jtag bt nvs_flash
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
