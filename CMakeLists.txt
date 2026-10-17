cmake_minimum_required(VERSION 3.16)
project(mounts VERSION 0.1.0)

find_package(Albert REQUIRED)
find_package(QCoro6 REQUIRED COMPONENTS Coro DBus)

albert_plugin(QT Core DBus Widgets)

target_link_libraries(${PROJECT_NAME} PRIVATE QCoro6::Coro QCoro6::DBus)
