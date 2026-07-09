cmake_minimum_required(VERSION 3.4.1)
project(demo LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -g -pthread)

# 项目路径（相对路径，不写死 /home/teamhd/）
set(PROJECT_ROOT ${CMAKE_SOURCE_DIR})
set(MPP_ROOT ${PROJECT_ROOT}/MPP)
set(INSTALL_PREFIX "${PROJECT_ROOT}/install")

# OpenCV：优先用系统安装的（apt install libopencv-dev），否则使用项目内 MPP/opencv4-1
set(USE_SYSTEM_OPENCV OFF)
find_package(OpenCV QUIET)
if(OpenCV_FOUND)
  set(USE_SYSTEM_OPENCV ON)
  message(STATUS "Using system OpenCV: ${OpenCV_VERSION}")
else()
  if(EXISTS "${MPP_ROOT}/opencv4-1/lib/libopencv_core.so")
    set(OPENCV_ROOT "${MPP_ROOT}/opencv4-1")
    message(STATUS "Using OpenCV from MPP: ${OPENCV_ROOT}")
  else()
    message(FATAL_ERROR
      "OpenCV not found.\n"
      "请在目标板上二选一：\n"
      "  1) 安装系统 OpenCV：sudo apt install libopencv-dev\n"
      "  2) 或在项目内提供 MPP/opencv4-1 目录（含 libopencv_core.so 等）")
  endif()
endif()

# eXosip 依赖 osipparser2，检查能否找到
set(OSIP_HEADER "")
if(EXISTS "${INSTALL_PREFIX}/include/osipparser2/osip_message.h")
  set(OSIP_HEADER "${INSTALL_PREFIX}/include/osipparser2/osip_message.h")
elseif(EXISTS "/usr/include/osipparser2/osip_message.h")
  set(OSIP_HEADER "/usr/include/osipparser2/osip_message.h")
elseif(EXISTS "/usr/local/include/osipparser2/osip_message.h")
  set(OSIP_HEADER "/usr/local/include/osipparser2/osip_message.h")
endif()
if(OSIP_HEADER STREQUAL "")
  message(FATAL_ERROR
    "找不到 osipparser2（eXosip 编译依赖）。请二选一：\n"
    "  1) 系统安装: sudo apt install libosip2-dev\n"
    "  2) 安装到 install 目录: 下载 libosip2 源码，\n"
    "     ./configure --prefix=${INSTALL_PREFIX} && make && make install")
endif()
message(STATUS "osipparser2: ${OSIP_HEADER}")

# 可执行文件 + 源文件
add_executable(demo
  main.cc
  ps_rtp.cc
  decode.cc
  gb28181.cc
  xml_utils.cc
  push.cc
  config.cc
)

# 头文件路径
target_include_directories(demo PRIVATE
  ${PROJECT_ROOT}
  ${INSTALL_PREFIX}/include
  ${MPP_ROOT}/mpp/include
  ${MPP_ROOT}/osal/inc
  /usr/include
  /usr/local/include
)
if(USE_SYSTEM_OPENCV)
  target_include_directories(demo PRIVATE ${OpenCV_INCLUDE_DIRS})
else()
  target_include_directories(demo PRIVATE ${OPENCV_ROOT}/include/opencv4)
endif()

# 链接目录（rpath-link 让链接器找到 libeXosip2 依赖的 libosip2.so.15 / libosipparser2.so.15）
target_link_directories(demo PRIVATE
  ${INSTALL_PREFIX}/lib
  ${MPP_ROOT}/lib
  /usr/lib
  /usr/local/lib
  /usr/lib/aarch64-linux-gnu
  /usr/lib/x86_64-linux-gnu
)
# rpath-link：解析 libeXosip2 依赖的 libosip2.so.15 / libosipparser2.so.15（优先 install/lib，其次系统）
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath-link,${INSTALL_PREFIX}/lib -Wl,-rpath-link,/usr/lib -Wl,-rpath-link,/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,/usr/lib/x86_64-linux-gnu")
if(NOT USE_SYSTEM_OPENCV)
  target_link_directories(demo PRIVATE ${OPENCV_ROOT}/lib)
endif()

# 链接库：eXosip + OpenCV（系统 OpenCV 无需额外 dc1394/jpeg/OpenEXR）
target_link_libraries(demo PRIVATE
  mpi_enc
  rockchip_mpp
  eXosip2
  osip2
  osipparser2
  pthread
)
if(USE_SYSTEM_OPENCV)
  target_link_libraries(demo PRIVATE ${OpenCV_LIBS})
else()
  target_link_libraries(demo PRIVATE
    opencv_highgui opencv_imgproc opencv_core opencv_imgcodecs opencv_videoio opencv_video
    dc1394 jpeg IlmImf Imath
  )
endif()

# 输出目录 + 运行时库搜索路径（install/lib 使他人拷贝项目后可直接运行 ./build/demo）
set_target_properties(demo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${PROJECT_ROOT}/build
  BUILD_RPATH "${INSTALL_PREFIX}/lib:/usr/lib:/usr/lib/aarch64-linux-gnu:/usr/lib/x86_64-linux-gnu"
  INSTALL_RPATH "${INSTALL_PREFIX}/lib:/usr/lib:/usr/lib/aarch64-linux-gnu:/usr/lib/x86_64-linux-gnu"
)
