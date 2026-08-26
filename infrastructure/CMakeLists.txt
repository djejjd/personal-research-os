add_library(pros_infrastructure STATIC
  src/command_composition.cpp
  src/file_operation_log.cpp
  src/governance_composition.cpp
  src/local_data_directory.cpp
  src/markdown_fidelity.cpp
  src/project_provisioning.cpp
  src/resource_resolver.cpp
  src/schema_migrator.cpp
  src/sqlite_approval_command_handler.cpp
  src/sqlite_command_transaction.cpp
  src/sqlite_work_command_handler.cpp
  src/sqlite_work_query_service.cpp
)

target_include_directories(pros_infrastructure PUBLIC include)
target_link_libraries(pros_infrastructure PUBLIC pros_application pros_domain SQLite3::SQLite3)
pros_enable_warnings(pros_infrastructure)
