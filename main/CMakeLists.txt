idf_component_register(SRC_DIRS "." "hardware" "network" "app" "ui" "ui/widgets" "ui/pages" "ui/fonts"
                       INCLUDE_DIRS "." "hardware" "network" "app" "ui" "ui/widgets"
                       EMBED_TXTFILES "network/remote_page.html"
                       # lv_mem_psram.c provides functions LVGL calls; keep them despite link order.
                       WHOLE_ARCHIVE)
