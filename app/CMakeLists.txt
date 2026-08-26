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

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/infrastructure/src/schema_migrator.cpp")
file(SHA256 "${PROJECT_SOURCE_DIR}/infrastructure/src/schema_migrator.cpp" PROS_SCHEMA_SOURCE_HASH)
string(SUBSTRING "${PROS_SCHEMA_SOURCE_HASH}" 0 12 PROS_SCHEMA_TEST_ID)
add_test(NAME app_smoke
  COMMAND personal_research_os --data-dir "${CMAKE_BINARY_DIR}/test-data/app-smoke-${PROS_SCHEMA_TEST_ID}" --smoke-test)
set_tests_properties(app_smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
