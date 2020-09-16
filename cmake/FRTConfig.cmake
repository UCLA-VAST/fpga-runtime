find_package(OpenCL REQUIRED)
find_package(TinyXML REQUIRED PATHS ${CMAKE_CURRENT_LIST_DIR})
find_package(XRT PATHS ${CMAKE_CURRENT_LIST_DIR})
find_package(SDx PATHS ${CMAKE_CURRENT_LIST_DIR})

include("${CMAKE_CURRENT_LIST_DIR}/FRTTargets.cmake")
