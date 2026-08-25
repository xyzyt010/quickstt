cmake_minimum_required(VERSION 3.16)
project(QtAwesomeSampleQml)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Qml Quick)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "")

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_subdirectory(../ QtAwesome)

#qt6_add_resources(QT_RESOURCES qml.qrc)
add_library(
	main.qrc
)

add_executable(QtAwesomeSampleQml
  main.cpp
  main.qrc
)

target_link_libraries(QtAwesomeSampleQml
  PUBLIC QtAwesome
  PRIVATE Qt::Qml Qt::Quick
)

