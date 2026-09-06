set(PRIV_REQUIRES_LIST device esp_matter esp_matter_console route_hook arduino driver)

idf_component_register(SRC_DIRS          ".;cc1101"
                      PRIV_INCLUDE_DIRS  ".;cc1101"
                      PRIV_REQUIRES      ${PRIV_REQUIRES_LIST})

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 14)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
