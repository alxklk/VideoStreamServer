CMAKE_MINIMUM_REQUIRED(VERSION 3.16)

PROJECT(main)

FIND_PACKAGE(PkgConfig REQUIRED)
find_package(OpenCV 4.6 REQUIRED)

pkg_check_modules(GST REQUIRED 
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-rtsp-server-1.0
)
include_directories(${GST_INCLUDE_DIRS})
link_directories(${GST_LIBRARY_DIRS})
add_definitions(${GST_CFLAGS_OTHER})


ADD_EXECUTABLE(main main.cpp)

target_include_directories(main
PRIVATE 
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(main 
PRIVATE
    ${OpenCV_LIBS}
    ${GST_LIBRARIES}
)

