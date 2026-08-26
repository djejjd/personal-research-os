cmake_minimum_required(VERSION 3.24)

project(PersonalResearchOS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

option(PERSONAL_RESEARCH_OS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

find_package(Qt6 6.11.1 EXACT REQUIRED COMPONENTS Core Widgets Test)
find_package(SQLite3 3.53.4 EXACT REQUIRED)

function(pros_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(PERSONAL_RESEARCH_OS_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(PERSONAL_RESEARCH_OS_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

enable_testing()

add_subdirectory(domain)
add_subdirectory(application)
add_subdirectory(infrastructure)
add_subdirectory(app)
add_subdirectory(tests)
