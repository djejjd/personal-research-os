add_library(pros_infrastructure STATIC
  src/local_data_directory.cpp
  src/schema_migrator.cpp
)

target_include_directories(pros_infrastructure PUBLIC include)
target_link_libraries(pros_infrastructure PUBLIC pros_application pros_domain SQLite3::SQLite3)
pros_enable_warnings(pros_infrastructure)
