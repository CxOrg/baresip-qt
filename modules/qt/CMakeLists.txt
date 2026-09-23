project(qt CXX)

# Prefer Qt6 (KDE Plasma 6 / current distros), fall back to Qt5.
find_package(Qt6 QUIET COMPONENTS Widgets DBus)
if(Qt6Widgets_FOUND)
  set(QT_VERSION_MAJOR 6)
  set(QT_LIBRARIES Qt6::Widgets Qt6::DBus)
  # KF6ConfigWidgets provides KStyleManager::initStyle() which
  # applies the user's configured KDE widget style (Breeze).
  find_package(KF6ConfigWidgets QUIET)
  if(KF6ConfigWidgets_FOUND)
    set(HAVE_KSTYLE ON)
  endif()
else()
  find_package(Qt5 QUIET COMPONENTS Widgets DBus)
  if(Qt5Widgets_FOUND)
    set(QT_VERSION_MAJOR 5)
    set(QT_LIBRARIES Qt5::Widgets Qt5::DBus)
    find_package(KF5ConfigWidgets QUIET)
    if(KF5ConfigWidgets_FOUND)
      set(HAVE_KSTYLE ON)
    endif()
  endif()
endif()

if(NOT QT_VERSION_MAJOR)
  return()
endif()

# LayerShellQt provides Wayland layer-shell positioning (tray-anchored
# popup panels on KDE Plasma). Optional -- falls back to plain
# frameless Tool window if not available.
find_package(LayerShellQt QUIET)
if(LayerShellQt_FOUND)
  set(HAVE_LAYERSHELL ON)
endif()

list(APPEND MODULES_DETECTED ${PROJECT_NAME})
set(MODULES_DETECTED ${MODULES_DETECTED} PARENT_SCOPE)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SRCS qt_mod.cpp tray_app.cpp call_dialog.cpp call_history.cpp settings_dialog.cpp)

if(STATIC)
    add_library(${PROJECT_NAME} OBJECT ${SRCS})
else()
    add_library(${PROJECT_NAME} MODULE ${SRCS})
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE ${QT_LIBRARIES})
if(HAVE_KSTYLE)
  if(QT_VERSION_MAJOR EQUAL 6)
    target_link_libraries(${PROJECT_NAME} PRIVATE KF6::ConfigWidgets)
  else()
    target_link_libraries(${PROJECT_NAME} PRIVATE KF5::ConfigWidgets)
  endif()
  target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_KSTYLE)
endif()
if(HAVE_LAYERSHELL)
  target_link_libraries(${PROJECT_NAME} PRIVATE LayerShellQtInterface)
  target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_LAYERSHELL)
endif()

target_compile_options(${PROJECT_NAME} PRIVATE
    -Wno-deprecated-declarations
)
