cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.1.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# LVGL allocates its object tree, styles, chart arrays and the on-screen
# keyboard through these hooks. Internal SRAM is the scarce resource on this
# board (task stacks, WiFi buffers, LCD bounce buffers all need it), so send
# LVGL to PSRAM with an internal fallback for when PSRAM is exhausted.
#
# The "prefer" variants take a caps list and fall through in order; the "2"
# is the number of capability sets that follow.
add_compile_options("-DLV_MEM_CUSTOM_ALLOC(s)=heap_caps_malloc_prefer(s,2,(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),MALLOC_CAP_8BIT)")
add_compile_options("-DLV_MEM_CUSTOM_FREE(p)=heap_caps_free(p)")
add_compile_options("-DLV_MEM_CUSTOM_REALLOC(p,s)=heap_caps_realloc_prefer(p,s,2,(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),MALLOC_CAP_8BIT)")

project(prometheus_panel)
