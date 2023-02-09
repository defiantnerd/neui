# CMakeList.txt : CMake project for neui, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.19)

project ("neui")

set(CMAKE_CXX_STANDARD 17)
 if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:__cplusplus /permissive- /Zc:twoPhase-")		
endif()
if(MSVC)
ADD_DEFINITIONS(-DUNICODE)
ADD_DEFINITIONS(-D_UNICODE)
endif()

include(neui/cmake/tools.cmake)

#[[ folders
	common				common files like headers needed for client and seats
	client				everything on the client side
	seat				everything for the seat side.

	common things are coordinates and some helper classes for the indexed tree 

]]

#[[
set(wintt neui/seat/wintt)
set(winttlibsources
	${wintt}/apihelper.h
	${wintt}/apihelper.cpp
	${wintt}/base.h
	${wintt}/base.cpp
	${wintt}/button.h
	${wintt}/button.cpp
	${wintt}/canvas.h
	${wintt}/canvas.cpp
	${wintt}/controls.h
	${wintt}/controls.cpp
	${wintt}/direct2d.h
	${wintt}/direct2d.cpp
	${wintt}/frame.h
	${wintt}/frame.cpp
	${wintt}/mainwindow.h
	${wintt}/mainwindow.cpp
	${wintt}/resources.h
	${wintt}/resources.cpp
	${wintt}/textedit.h
	${wintt}/textedit.cpp
)

add_library(wintt
	${winttlibsources}
)

]]


set(commonsources
	neui/common/indexedwidgettree.h
	neui/common/common.h
	neui/common/color.h
	neui/common/geometry.h
	neui/common/bitmap.h
	neui/common/renderer.h
	neui/common/style.h
	neui/common/boxmodel.h
	neui/common/layoutable.h
	neui/common/widgetbase.h	
	neui/common/widget.h
	neui/common/mujson.h
	neui/common/mujson.cpp
)

add_library(commonlib
  ${commonsources}
)

# --------------------------
set(d2dfiles
	neui/seat/gfx/d2d/direct2d.h
	neui/seat/gfx/d2d/direct2d.cpp
	)
# --------------------------


set(winlightsources
    neui/seat/winlight/base.h
	neui/seat/winlight/base.cpp	
	neui/seat/winlight/seatimpl.h
	neui/seat/winlight/seatimpl.cpp
)

add_library(winlight
  ${commonsources}
  ${winlightsources}
)


#target_link_libraries(winlight PRIVATE fmt::fmt)
target_link_libraries(winlight PRIVATE commonlib)

target_include_directories(winlight
  PRIVATE
  neui
)

target_compile_definitions(winlight INTERFACE NOMINMAX)

set(wind2dsources	
	${d2dfiles}

	neui/seat/wind2d/controls.h
	neui/seat/wind2d/base.h
	neui/seat/wind2d/base.cpp	
	neui/seat/wind2d/seatimpl.h
	neui/seat/wind2d/seatimpl.cpp
	neui/seat/base/widgetfactory.h
	neui/seat/base/widgetfactory.cpp
	neui/seat/wind2d/seatcontrols.h
	neui/seat/wind2d/seatcontrols.cpp
	neui/seat/wind2d/appwindow.h
	neui/seat/wind2d/appwindow.cpp
	neui/seat/wind2d/staticcontrols.h
	neui/seat/wind2d/staticcontrols.cpp
	#neui/seat/wind2d/wic.h
	#neui/seat/wind2d/wic.cpp
	#neui/seat/wind2d/bitmaps.h
	#neui/seat/wind2d/bitmaps.cpp
)



add_library(wind2d  
	# ${commonsources}
	${wind2dsources}
)


# target_link_libraries(wind2d PRIVATE fmt::fmt)
target_link_libraries(wind2d PRIVATE commonlib)


target_include_directories(wind2d
  PRIVATE
  neui
)

target_compile_definitions(wind2d INTERFACE NOMINMAX)

set(neui_sources
	neui/neui.h
	neui/neui.cpp
	
	neui/seat/bitmap.h
	neui/seat/bitmap.cpp
	neui/seat/resource.h
	neui/seat/seat.h
	neui/seat/seat.cpp
	neui/common/events.h
	neui/common/contents.h
	neui/common/contents.cpp

	#neui/seat/base/baseseat.h
	#neui/seat/base/indexedwidgettree.h
	# neui/seat/base/indexedwidgettree.cpp
	#neui/seat/base/widgetfactory.h
	#neui/seat/base/widgetfactory.cpp
	#neui/seat/base/basewidget.h

	neui/client/commonwidgets.h
	neui/client/commonwidgets.cpp
	neui/common/widgetbase.h
	neui/common/widgetbase.cpp

)

# Add source to this project's executable.
add_library (neui
	${neui_sources}
)

target_link_libraries(neui PRIVATE wind2d)
# target_link_libraries(neui PRIVATE winlight)
target_link_libraries(neui PRIVATE commonlib)
#target_link_libraries(neui PRIVATE fmt::fmt)

target_compile_definitions(neui INTERFACE NOMINMAX)
target_compile_definitions(commonlib INTERFACE NOMINMAX)

target_include_directories(neui
  PRIVATE
  neui
)

add_executable(main WIN32
	main.cpp
)

declare_dpiawareness(main)
compatible_with_win10(main)

target_include_directories(main
  PRIVATE
  neui
)


# find_package(fmt CONFIG REQUIRED)

# main -> neui -> wind2d
#         |       |
#         fmt     fmt

target_link_libraries(main PRIVATE neui)

if (WIN32)
target_link_options(main PRIVATE -mwindows)
endif()


# TODO: Add tests and install targets if needed.
