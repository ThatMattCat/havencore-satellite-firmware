set(UI_DIR ./ui)
file(GLOB_RECURSE UI_SRCS ${UI_DIR}/*.c)

set(APP_DIR ./app)
file(GLOB_RECURSE APP_SRCS ${APP_DIR}/*.c)

set(SETTINGS_DIR ./settings)
file(GLOB_RECURSE SETTINGS_SRCS ${SETTINGS_DIR}/*.c)

idf_component_register(
    SRCS
        "main.c"
        ${UI_SRCS}
        ${APP_SRCS}
        ${SETTINGS_SRCS}
    INCLUDE_DIRS
        "."
        ${UI_DIR}
        ${APP_DIR}
        ${SETTINGS_DIR}
)

set(MV_UF2_BIN_EXE ${COMPONENT_PATH}/generateuf2.py)
idf_build_get_property(build_dir BUILD_DIR)
set(nvs_src_file ${PROJECT_DIR}/factory_nvs/build/factory_nvs.bin)
set(nvs_dst_file ${build_dir}/uf2/factory_nvs.bin)

if(NOT EXISTS ${nvs_src_file})
    message(FATAL_ERROR "Please ensure that the 'factory_nvs' is built first, and make sure to keep the 'HMI Board Config' consistent.")
endif()

add_custom_target(nvs_uf2_bin ALL)
add_custom_command(
        TARGET nvs_uf2_bin
        COMMAND ${CMAKE_COMMAND} -E make_directory ${build_dir}/uf2
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${nvs_src_file} ${nvs_dst_file}
        COMMAND ${CMAKE_COMMAND} -E echo "Copying factory_nvs.bin to destination if it is different..."
        VERBATIM)

add_dependencies(flash nvs_uf2_bin)

# Partition layout: `factory` holds TinyUF2 (recovery image, never
# overwritten by OTA); main app lives in `ota_0` (and OTA-cycles into
# `ota_1`). IDF's `esptool_py/CMakeLists.txt` unconditionally adds the
# project bin at `parttool.py --partition-boot-default`'s offset to the
# `flash` target, and that prefers the factory partition whenever one
# exists — but in our layout the factory offset is TinyUF2's home.
# `esptool` refuses duplicate offsets in the flash args, so we have to
# actually strip IDF's stale `app → 0x10000` entry from the three
# target properties (FLASH_FILE / FLASH_ENTRY / IMAGES) that
# `esptool_py_flash_target_image()` populates, then add our own
# entries. The image_name "app" is unique to IDF's auto-add; ours use
# the partition names ("factory", "ota_0").
get_property(_idf_flash_file  TARGET flash PROPERTY FLASH_FILE)
get_property(_idf_flash_entry TARGET flash PROPERTY FLASH_ENTRY)
get_property(_idf_flash_imgs  TARGET flash PROPERTY IMAGES)
set(_idx 0)
set(_strip -1)
foreach(_entry IN LISTS _idf_flash_entry)
    if(_entry MATCHES "^\"app\"")
        set(_strip ${_idx})
        break()
    endif()
    math(EXPR _idx "${_idx} + 1")
endforeach()
if(NOT _strip EQUAL -1)
    list(REMOVE_AT _idf_flash_file  ${_strip})
    list(REMOVE_AT _idf_flash_entry ${_strip})
    list(REMOVE_AT _idf_flash_imgs  ${_strip})
    set_property(TARGET flash PROPERTY FLASH_FILE  "${_idf_flash_file}")
    set_property(TARGET flash PROPERTY FLASH_ENTRY "${_idf_flash_entry}")
    set_property(TARGET flash PROPERTY IMAGES      "${_idf_flash_imgs}")
endif()

esptool_py_flash_to_partition(flash "factory" "${nvs_dst_file}")
esptool_py_flash_to_partition(flash "ota_0" "${build_dir}/${CMAKE_PROJECT_NAME}.bin")

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
spiffs_create_partition_image(model ../model FLASH_IN_PROJECT)

# Auto-publish: after the .bin is generated, scp it to the HavenCore web
# server so Settings -> Update Firmware can pull it. No-op unless
# .publish.env exists at the repo root with HC_PUBLISH_DEST set; failures
# are non-fatal. The stamp file gates re-runs on .bin changes only.
set(_app_bin "${build_dir}/${CMAKE_PROJECT_NAME}.bin")
set(_publish_stamp "${build_dir}/.firmware_published")
add_custom_command(
    OUTPUT ${_publish_stamp}
    DEPENDS ${_app_bin}
    COMMAND bash ${PROJECT_DIR}/scripts/publish_firmware.sh ${_app_bin}
    COMMAND ${CMAKE_COMMAND} -E touch ${_publish_stamp}
    VERBATIM)
add_custom_target(publish_firmware ALL DEPENDS ${_publish_stamp})
add_dependencies(publish_firmware gen_project_binary)
