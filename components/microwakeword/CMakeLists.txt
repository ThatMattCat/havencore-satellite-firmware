idf_component_register(
    SRCS
        "microwakeword.c"
        "manifest.c"
        "frontend_wrap.cc"
        "streaming_model.cc"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
    REQUIRES
        esp_common
    PRIV_REQUIRES
        esp-tflite-micro
        json
        spi_flash
        esp_ringbuf
        esp_timer
        freertos
        heap
        log
)

# Silence -Wunused-function in TFLM's vendored kernels when included
# from our C++ wrapper. Applies to this component only.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-missing-field-initializers
)
