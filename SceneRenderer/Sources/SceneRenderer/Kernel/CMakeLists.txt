# ---------------------------------------------------------------------------
# SceneRendererCore — small foundational utilities, JSON/VFS adapters, and
# external C++ module facades.
# ---------------------------------------------------------------------------
add_library(SceneRendererCore STATIC)
target_sources(SceneRendererCore PRIVATE Json.cpp)
target_sources(SceneRendererCore
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES
         Core.cppm
         eigen.cppm
         Fs.cppm
         nlohmann_json.cppm
         Json.cppm)
target_link_libraries(SceneRendererCore
  PUBLIC rstd::rstd rstd.cppstd rstd.log Eigen3::Eigen nlohmann_json)
target_compile_features(SceneRendererCore PUBLIC cxx_std_20)
target_compile_options(SceneRendererCore PRIVATE -Wno-missing-field-initializers)
set_property(TARGET SceneRendererCore PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET SceneRendererCore PROPERTY CXX_SCAN_FOR_MODULES ON)


# ---------------------------------------------------------------------------
# SceneRendererTypes — minimal shared value types. Kept with Foundation so
# rendering and import layers can depend on it without pulling in runtime code.
# ---------------------------------------------------------------------------
add_library(SceneRendererTypes STATIC)
target_sources(SceneRendererTypes
  PUBLIC FILE_SET cxx_modules TYPE CXX_MODULES FILES Types/types.cppm)
target_sources(SceneRendererTypes PRIVATE Types/types.cpp)
target_link_libraries(SceneRendererTypes
  PUBLIC rstd::rstd rstd.cppstd SceneRendererCore)
target_include_directories(SceneRendererTypes PUBLIC ..)
target_compile_features(SceneRendererTypes PUBLIC cxx_std_20)
target_compile_options(SceneRendererTypes PRIVATE -Wno-missing-field-initializers)
set_property(TARGET SceneRendererTypes PROPERTY POSITION_INDEPENDENT_CODE ON)
set_property(TARGET SceneRendererTypes PROPERTY CXX_SCAN_FOR_MODULES ON)
