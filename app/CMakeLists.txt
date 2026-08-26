qt_add_executable(personal_research_os
  main.cpp
)

target_link_libraries(personal_research_os PRIVATE
  Qt6::Core
  Qt6::Widgets
  pros_application
  pros_infrastructure
)
pros_enable_warnings(personal_research_os)
