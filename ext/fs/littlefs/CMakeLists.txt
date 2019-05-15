add_library(LITTLEFS INTERFACE)

target_include_directories(LITTLEFS INTERFACE src include)

zephyr_compile_definitions(LFS_CONFIG=lfs_config.h)

zephyr_library()
zephyr_library_sources(
    src/lfs.c
    src/lfs_util.c
    )
zephyr_library_link_libraries(LITTLEFS)
target_link_libraries(LITTLEFS INTERFACE zephyr_interface)
