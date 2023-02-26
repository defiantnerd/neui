# neui win32 functions
#
# select OS compatibility in manifest and dpi awareness
#
# the manifests only work on WIN32, but the functions do it by themselves

function(declare_dpiawareness target)
if(WIN32)
  target_sources(${target} PRIVATE
	neui/res/win32manifests/dpiaware.manifest
)
endif()
endfunction()

function(compatible_with_win7 target)
if(WIN32)
  target_sources(${target} PRIVATE
     neui/res/win32manifests/compatibility_win7.manifest
)
endif()
endfunction()

function(compatible_with_win8 target)
if(WIN32)
  target_sources(${target} PRIVATE
     neui/res/win32manifests/compatibility_win8.manifest
  )
endif()
endfunction()

function(compatible_with_win81 target)
if(WIN32)
  target_sources(${target} PRIVATE
     neui/res/win32manifests/compatibility_win81.manifest
  )
endif()
endfunction()

function(compatible_with_win10 target)
if(WIN32)
  target_sources(${target} PRIVATE
     neui/res/win32manifests/compatibility_win10.manifest
  )
endif()
endfunction()
