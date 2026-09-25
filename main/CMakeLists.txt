idf_component_register(SRC_DIRS          "." "cc1101"
                       PRIV_INCLUDE_DIRS "." "cc1101" "${ESP_MATTER_PATH}/examples/common/utils")

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
