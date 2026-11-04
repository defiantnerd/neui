# Shader executor benchmark (report only): times the real .nsl compiler +
# executor (neui-shader) on a few programs. Correctness and the cross-platform
# goldens are in neui_tests. Run ./tests/shader_bench/<config>/neui_shader_bench.
add_executable(neui_shader_bench shader_bench.cpp)
target_compile_features(neui_shader_bench PRIVATE cxx_std_17)
target_include_directories(neui_shader_bench PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(neui_shader_bench PRIVATE neui-shader)
