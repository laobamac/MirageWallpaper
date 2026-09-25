add_library(rstd.cppstd STATIC)
target_compile_features(rstd.cppstd PUBLIC cxx_std_20)
target_sources(
  rstd.cppstd
  PUBLIC FILE_SET
         modules
         TYPE
         CXX_MODULES
         FILES
         rstd.cppm
         cppstd.cppm)

target_link_libraries(rstd.cppstd PUBLIC rstd.std fmt::fmt)
