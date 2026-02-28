cmake_minimum_required(VERSION 3.8)
project(yolo_ros)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(cv_bridge REQUIRED)
find_package(OpenCV REQUIRED)

if(NOT DEFINED ONNXRUNTIME_ROOT)
  if(DEFINED ENV{ONNXRUNTIME_ROOT})
    set(ONNXRUNTIME_ROOT $ENV{ONNXRUNTIME_ROOT})
  else()
    set(ONNXRUNTIME_ROOT /opt/onnxruntime)
  endif()
endif()

find_path(
  ONNXRUNTIME_INCLUDE_DIR
  NAMES onnxruntime_cxx_api.h
  PATHS ${ONNXRUNTIME_ROOT}/include /opt/onnxruntime/include
  REQUIRED)

find_library(
  ONNXRUNTIME_LIB
  NAMES onnxruntime
  PATHS ${ONNXRUNTIME_ROOT}/lib /opt/onnxruntime/lib
  REQUIRED)

add_executable(
  yolo_infer_gui_node
  src/yolo_infer_gui_node.cpp
  src/yolo_onnx.cpp)

ament_target_dependencies(yolo_infer_gui_node rclcpp sensor_msgs cv_bridge)

target_include_directories(
  yolo_infer_gui_node
  PRIVATE include ${ONNXRUNTIME_INCLUDE_DIR} ${OpenCV_INCLUDE_DIRS})

target_link_libraries(yolo_infer_gui_node ${OpenCV_LIBS} ${ONNXRUNTIME_LIB})

install(TARGETS yolo_infer_gui_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY include DESTINATION include)
install(DIRECTORY launch model DESTINATION share/${PROJECT_NAME})

ament_package()
