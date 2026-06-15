cmake_minimum_required(VERSION 3.15)

if(WIN32)
  add_library(neui-backend-d2d d2d_backend.cpp)

  target_include_directories(neui-backend-d2d
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC  ${PROJECT_SOURCE_DIR}/include
  )

  target_link_libraries(neui-backend-d2d PRIVATE d2d1)
endif()
