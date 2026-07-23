# RmskinViewer — standalone debug window for a single .rmskin theme / layout.

add_executable(RmskinViewer RmskinViewer.mm)

target_link_libraries(RmskinViewer PRIVATE
    RmskinRendererEngine
    ${RM_FRAMEWORKS})

target_compile_features(RmskinViewer PRIVATE cxx_std_20)
set_target_properties(RmskinViewer PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
target_compile_options(RmskinViewer PRIVATE -Wno-deprecated-declarations)
