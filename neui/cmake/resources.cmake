# generic resource handling for targets
# (c) 2023 Timo Kaluza
# version 1.1
#
# yes, there are comments :)

set(_dn_targets_with_resources "") 

## Things to be done at the very end of configure phase
## as if they would be at bottom of CMakelists.txt
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL _dn_final_collection_and_resource_build_phase())

## configure the resource files
# called after the CMakeLists.txt file has been processed
# for each target that collected resources an appropriate
# resource handling will be generated
function(_dn_final_collection_and_resource_build_phase)
   # message(STATUS "list of targets: ${_dn_targets_with_resources}")
   if (NOT ${_dn_targets_with_resources} STREQUAL "")
     message(STATUS "generating resources for targets")
     list(REMOVE_DUPLICATES _dn_targets_with_resources)

     foreach(targetname IN ITEMS ${_dn_targets_with_resources})
     if (WIN32)
       message(STATUS "generating for target ${targetname}")
       _win32_generate_rc_file_for_target(${targetname})
     elseif(APPLE)
       message(FATAL_ERROR "implement macOS bundle stuff")
     elseif(UNIX)
       message(FATAL_ERROR "implement unix/linux bundle stuff")
     else()
       message(FATAL_ERROR "implement whatever you are stuff")
     endif()
     
     endforeach()
   endif()
endfunction(_dn_final_collection_and_resource_build_phase)

function(add_target_resource TARGET ) 
  if (NOT TARGET ${TRG})
    message(FATAL_ERROR "unknown target ${TRG}")
  endif()

  # registering the target when used the first time
  if ( NOT ${TARGET} IN_LIST _dn_targets_with_resources)
    # message(STATUS "adding ${TARGET} into list with resources")
    list(APPEND _dn_targets_with_resources ${TARGET} )
    set(_dn_targets_with_resources ${_dn_targets_with_resources} PARENT_SCOPE)
  endif()

  # getting the property from the target
  get_target_property(RSL ${TARGET} RESLIST)
  if ( RSL STREQUAL "RSL-NOTFOUND")
    # target hadn't had any resource, now adding resource file and property
    set(RSL)
  endif()

  # adding all resource names to the list
  foreach(res ${ARGN} )
    list(APPEND RSL ${res})
  endforeach()

  # updating the target property
  set_target_properties(${TARGET} PROPERTIES RESLIST "${RSL}")
endfunction()

# adds all files in a directory which filenames start with a "a-z|A-Z|0-9",
# so files that should be ignored should use "_" or numbers in front
function(add_target_resourcedir IN_TARGET IN_PATH) 
    string(LENGTH ${IN_PATH} len)
    math(EXPR len "${len}-1")
    string(SUBSTRING ${IN_PATH} ${len} -1 last)
    if ("${last}" STREQUAL "/")
      string(SUBSTRING ${IN_PATH} 0 ${len} IN_PATH)
      message(STATUS "removing trailing / -> ${IN_PATH}")
    endif()
    file(GLOB filelist "${CMAKE_CURRENT_SOURCE_DIR}/${IN_PATH}/*.*" )

    foreach(res ${filelist})
      get_filename_component(res_filename ${res} NAME)
      string(SUBSTRING ${res} 0 1 firstchar)
      string(REGEX MATCH "[a-z]|[A-Z]|[0-9]" matched "${firstchar}")
      if (matched)
        add_target_resource(${IN_TARGET} ${IN_PATH}/${res_filename})
      endif()
    endforeach()
endfunction()

function(_dn_win32_generate_rc_file_for_target OUT_TYPE IN_SUFFIX)
  # the file suffix will determine the resource type
  string(TOUPPER ${IN_SUFFIX} IN_SUFFIX)
  string(REPLACE "." "" IN_SUFFIX ${IN_SUFFIX} )
  set(result "\"${IN_SUFFIX}\"")
  # translating some suffices to a rescompiler type
  if(${IN_SUFFIX} STREQUAL "TTF")
    set(result "\"FONT\"")
  endif()
  if(${IN_SUFFIX} STREQUAL "BMP")
    set(result "BITMAP")
  endif()
  if(${IN_SUFFIX} STREQUAL "ICN")
    set(result "ICON")
  endif()
  if(${IN_SUFFIX} STREQUAL "sql3")
    set(result "RT_DB")
  endif()
  # return ${result}
  set(${OUT_TYPE} ${result} PARENT_SCOPE)
endfunction(_dn_win32_generate_rc_file_for_target)

# generator for win32 PE embedded resources
function (_win32_generate_rc_file_for_target targetname)
   set(target_rc_file "resources_${targetname}.rc")
   # message("target file: ${target_rc_file}")
   get_target_property(RSL ${targetname} RESLIST)
   set(SOMERES "")
   list(REMOVE_DUPLICATES RSL)
   #message(STATUS "resource list is ${RSL}")
   foreach(res ${RSL})
      # cmake 3.20:
      # cmake_path(GET res FILENAME res_filename)
      # cmake_path(GET res EXTENSION LASTONLY res_suffix)
      get_filename_component(res_filename ${res} NAME)
      get_filename_component(res_suffix ${res} EXT)
      _dn_win32_generate_rc_file_for_target(restype ${res_suffix})

      set(newtext "${res_filename}\t\t\t${restype}\t\t\"${CMAKE_SOURCE_DIR}/${res}\"")
      # message(STATUS "generated line: ${newtext}")
      set(SOMERES "${SOMERES}\n${newtext}")
   endforeach()
   message(STATUS "writing ${target_rc_file} to ${CMAKE_CURRENT_BINARY_DIR}")
   file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${target_rc_file} "
#include <winres.h>

${SOMERES}
   ")
   target_sources(${targetname} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${target_rc_file})
endfunction(_win32_generate_rc_file_for_target)
