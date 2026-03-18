idf_component_register(
    SRCS "src/vterm.c" "src/parser.c" "src/state.c" "src/screen.c"
         "src/pen.c" "src/keyboard.c" "src/mouse.c" "src/encoding.c"
         "src/unicode.c"
    INCLUDE_DIRS "include"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-maybe-uninitialized
    -Wno-implicit-fallthrough
    -Wno-empty-body
)
