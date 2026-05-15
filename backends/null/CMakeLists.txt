cmake_minimum_required(VERSION 3.15)

if(NOT WIN32)
  add_library(neui-backend-null null_backend.cpp)

  target_include_directories(neui-backend-null
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
  )
endif()
