cmake_minimum_required(VERSION 2.6.0)

# pull in the pods macros. See cmake/pods.cmake for documentation
set(POD_NAME image-compress)
include(cmake/pods.cmake)

# automatically build LCM types.  This also defines a number of CMake
# variables, see cmake/lcmtypes.cmake for details
include(cmake/lcmtypes.cmake)
lcmtypes_build()


add_subdirectory(src/image_io_utils)
add_subdirectory(src/camera_params)

	
#add_subdirectory(src/image-tool)
#add_subdirectory(src/tools)

# Old tools from Matt:
#add_subdirectory(src/image-comms)
#add_subdirectory(src/image-warp)
#add_subdirectory(src/image-anaglyph)
