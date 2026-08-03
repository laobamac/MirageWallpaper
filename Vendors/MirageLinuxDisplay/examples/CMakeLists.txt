add_executable(mirage_headless_consumer headless_consumer.c)
target_link_libraries(mirage_headless_consumer PRIVATE mirage_display_static)
target_compile_options(mirage_headless_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(mirage_mock_broker mock_broker.c)
target_link_libraries(mirage_mock_broker PRIVATE mirage_display_static)
target_include_directories(mirage_mock_broker PRIVATE
    ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/src/common)
target_compile_options(mirage_mock_broker PRIVATE -Wall -Wextra -Wpedantic -Werror)
