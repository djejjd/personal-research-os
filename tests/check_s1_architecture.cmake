file(GLOB_RECURSE PRODUCTION_SOURCES
  "${PROS_SOURCE_DIR}/app/*.cpp"
  "${PROS_SOURCE_DIR}/app/*.h"
  "${PROS_SOURCE_DIR}/application/*.cpp"
  "${PROS_SOURCE_DIR}/application/*.h"
  "${PROS_SOURCE_DIR}/domain/*.cpp"
  "${PROS_SOURCE_DIR}/domain/*.h"
  "${PROS_SOURCE_DIR}/infrastructure/*.cpp"
  "${PROS_SOURCE_DIR}/infrastructure/*.h")

foreach(SOURCE_FILE IN LISTS PRODUCTION_SOURCES)
  file(READ "${SOURCE_FILE}" SOURCE_CONTENT)
  if(SOURCE_CONTENT MATCHES "class[ \t\r\n]+ExecutionPort|QNetworkAccessManager|QProcess|TerminalAdapter|AutomationAdapter")
    message(FATAL_ERROR "S1 禁止的执行、网络、终端或自动化能力出现在 ${SOURCE_FILE}")
  endif()
endforeach()

file(READ "${PROS_SOURCE_DIR}/app/main.cpp" APP_SOURCE)
if(APP_SOURCE MATCHES "sqlite_(work|approval)_command_handler|governance_command_handler")
  message(FATAL_ERROR "应用组合根绕过了统一 CommandFacade")
endif()
