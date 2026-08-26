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

add_test(NAME app_smoke
  COMMAND personal_research_os --data-dir "${CMAKE_BINARY_DIR}/test-data/app-smoke" --smoke-test)
set_tests_properties(app_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
