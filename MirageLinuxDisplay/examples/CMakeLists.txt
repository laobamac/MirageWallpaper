add_executable(mirage_headless_consumer headless_consumer.cpp)
target_link_libraries(mirage_headless_consumer PRIVATE mirage_display_static)
target_compile_features(mirage_headless_consumer PRIVATE cxx_std_20)
target_compile_options(mirage_headless_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(mirage_mock_broker mock_broker.cpp)
target_link_libraries(mirage_mock_broker PRIVATE mirage_display_static)
target_compile_features(mirage_mock_broker PRIVATE cxx_std_20)
target_include_directories(mirage_mock_broker PRIVATE
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/common)
target_compile_options(mirage_mock_broker PRIVATE -Wall -Wextra -Wpedantic -Werror)
