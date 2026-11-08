SET(SOURCES screens/ui_Screen1.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/low_gas_icon_no_bg.c
    images/ui_black_boot_png.c
    images/ui_icon_fuel.c
    images/ui_icon_coolant.c
    fonts/ui_font_letters_36.c
    fonts/ui_font_value_66.c
    fonts/ui_font_caption_20.c
    fonts/ui_font_hub_32.c)

add_library(ui ${SOURCES})
