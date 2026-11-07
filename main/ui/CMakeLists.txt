SET(SOURCES screens/ui_Screen1.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/low_gas_icon_no_bg.c
    images/ui_black_boot_png.c
    images/ui_img_263778233.c)

add_library(ui ${SOURCES})
