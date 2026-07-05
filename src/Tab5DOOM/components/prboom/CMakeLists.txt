file(GLOB PRBOOM_SRCS "*.c")

idf_component_register(
    SRCS ${PRBOOM_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES prboom-wad-tables
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=char-subscripts
    -Wno-error=unused-value
    -Wno-error=parentheses
    -Wno-error=int-to-pointer-cast
    -Wno-pointer-sign
    -Wno-error=unused-but-set-parameter
    -Wno-error=maybe-uninitialized
    -Wno-error=nonnull
    -Wno-error=misleading-indentation
    -Wno-error=duplicate-decl-specifier
    -Wno-error=dangling-else
    -Wno-error=format-overflow
    -Wno-error=unused-const-variable
    -Wno-error=address
    -Wno-error=sizeof-pointer-div
)
