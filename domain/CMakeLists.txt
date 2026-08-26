add_library(pros_domain STATIC
  src/command_contract.cpp
  src/governance.cpp
  src/work_aggregates.cpp
)

target_include_directories(pros_domain INTERFACE include)
target_include_directories(pros_domain PUBLIC include)
pros_enable_warnings(pros_domain)
