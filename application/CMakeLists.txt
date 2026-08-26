add_library(pros_application STATIC
  src/app_config.cpp
)

target_include_directories(pros_application PUBLIC include)
target_link_libraries(pros_application PUBLIC Qt6::Core pros_domain)
pros_enable_warnings(pros_application)
