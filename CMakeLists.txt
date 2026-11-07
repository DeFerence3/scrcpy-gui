cmake_minimum_required(VERSION 3.21)
set(SCRCPY_VERSION "0.0.2" CACHE STRING "Application and distribution version")
project(scrcpy-ui VERSION ${SCRCPY_VERSION} LANGUAGES CXX)

find_package(Slint QUIET)
if (NOT Slint_FOUND)
  if (NOT WIN32)
    # Keep the Linux build small: no Qt backend (it would load Qt at runtime),
    # no GPU renderers. Winit drives Wayland/X11 and the software renderer
    # avoids loading GL drivers, which cost ~90 MB RSS for this small panel.
    set(SLINT_FEATURE_BACKEND_QT OFF CACHE BOOL "")
    set(SLINT_FEATURE_RENDERER_SKIA OFF CACHE BOOL "")
    set(SLINT_FEATURE_RENDERER_FEMTOVG OFF CACHE BOOL "")
    set(SLINT_FEATURE_RENDERER_SOFTWARE ON CACHE BOOL "")
  endif()
  message("Slint could not be located in the CMake module search path. Downloading it from Git and building it locally")
  include(FetchContent)
  FetchContent_Declare(
    Slint
    GIT_REPOSITORY https://github.com/slint-ui/slint.git
    # `release/1` will auto-upgrade to the latest Slint >= 1.0.0 and < 2.0.0
    # `release/1.0` will auto-upgrade to the latest Slint >= 1.0.0 and < 1.1.0
    GIT_TAG release/1
    SOURCE_SUBDIR api/cpp
  )
  FetchContent_MakeAvailable(Slint)
endif (NOT Slint_FOUND)

# Embed compiler debug symbols in object files to avoid compiler PDB API
# failures (C1090) on vc*.pdb. The linker still produces the final app PDB.
# CMake 3.21 stores /Zi in these flags (the format property requires 3.25).
if(MSVC)
  foreach(config IN ITEMS DEBUG RELWITHDEBINFO)
    string(REGEX REPLACE "(^|[ \t])[-/]Z[iI]([ \t]|$)" "\\1/Z7\\2"
      CMAKE_CXX_FLAGS_${config} "${CMAKE_CXX_FLAGS_${config}}")
  endforeach()
endif()

find_package(Threads REQUIRED)
set(SCRCPY_CORE_SOURCES src/core/AppCore.cpp src/core/Json.cpp src/platform/Platform.cpp src/device/DeviceFiles.cpp)
if(WIN32)
  list(APPEND SCRCPY_CORE_SOURCES src/platform/windows/Platform_win.cpp src/platform/windows/FileDropTarget.cpp)
  set(SCRCPY_DESKTOP_SOURCES src/platform/windows/Desktop_win.cpp)
else()
  list(APPEND SCRCPY_CORE_SOURCES src/platform/linux/Platform_posix.cpp src/platform/linux/Updater_posix.cpp)
  set(SCRCPY_DESKTOP_SOURCES src/platform/linux/Desktop_linux.cpp)
  # sd-bus talks to the tray host and xdg-desktop-portal without a toolkit;
  # libwayland-client receives file drops on Slint's Wayland connection.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
  pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
endif()
add_library(scrcpy-core STATIC ${SCRCPY_CORE_SOURCES})
target_compile_features(scrcpy-core PUBLIC cxx_std_17)
# Includes are written relative to src/, e.g. "platform/Platform.h".
target_include_directories(scrcpy-core PUBLIC src)
target_link_libraries(scrcpy-core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(scrcpy-core PUBLIC shell32 ole32 uuid)
endif()
add_executable(scrcpy-ui WIN32 src/main.cpp src/scrcpy/Scrcpy.cpp src/device/FileExplorer.cpp ${SCRCPY_DESKTOP_SOURCES})
set_target_properties(scrcpy-ui PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/app")
target_compile_features(scrcpy-ui PRIVATE cxx_std_17)
target_compile_definitions(scrcpy-ui PRIVATE APP_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(scrcpy-core PUBLIC /utf-8)
endif()

# Ninja remembers cl.exe, but does not retain the Developer Prompt's SDK
# environment. Restore it for compilation and linking from ordinary shells.
if (MSVC AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
  get_filename_component(MSVC_SEARCH_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
  while (MSVC_SEARCH_DIR)
    if (EXISTS "${MSVC_SEARCH_DIR}/Common7/Tools/VsDevCmd.bat")
      set(MSVC_DEV_CMD "${MSVC_SEARCH_DIR}/Common7/Tools/VsDevCmd.bat")
      break()
    endif()
    get_filename_component(MSVC_PARENT_DIR "${MSVC_SEARCH_DIR}" DIRECTORY)
    if (MSVC_PARENT_DIR STREQUAL MSVC_SEARCH_DIR)
      break()
    endif()
    set(MSVC_SEARCH_DIR "${MSVC_PARENT_DIR}")
  endwhile()
  if (MSVC_DEV_CMD AND CMAKE_CXX_COMPILER MATCHES "/bin/Host([^/]+)/([^/]+)/cl.exe$")
    set(MSVC_HOST_ARCH "${CMAKE_MATCH_1}")
    set(MSVC_TARGET_ARCH "${CMAKE_MATCH_2}")
    configure_file(cmake/msvc-env.cmd.in msvc-env.cmd @ONLY)
    set_target_properties(scrcpy-ui PROPERTIES
      CXX_COMPILER_LAUNCHER "${CMAKE_CURRENT_BINARY_DIR}/msvc-env.cmd"
      CXX_LINKER_LAUNCHER "${CMAKE_CURRENT_BINARY_DIR}/msvc-env.cmd")
    set_target_properties(scrcpy-core PROPERTIES
      CXX_COMPILER_LAUNCHER "${CMAKE_CURRENT_BINARY_DIR}/msvc-env.cmd")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_CURRENT_BINARY_DIR}/msvc-env.cmd")
    set(CMAKE_CXX_LINKER_LAUNCHER "${CMAKE_CURRENT_BINARY_DIR}/msvc-env.cmd")
  endif()
endif()

add_library(scrcpy-widgets STATIC)
# Ship the explorer icons inside the binary, including portable installations.
set_target_properties(scrcpy-widgets PROPERTIES SLINT_EMBED_RESOURCES embed-files)
target_link_libraries(scrcpy-widgets PUBLIC Slint::Slint)
if(MSVC)
  target_compile_options(scrcpy-widgets PRIVATE /utf-8)
endif()
slint_target_sources(scrcpy-widgets ui/main.slint LIBRARY_PATHS material=${CMAKE_CURRENT_SOURCE_DIR}/material-1.0/material.slint)
target_link_libraries(scrcpy-ui PRIVATE scrcpy-widgets scrcpy-core)
if(WIN32)
  target_link_libraries(scrcpy-ui PRIVATE ole32 uuid)
else()
  target_link_libraries(scrcpy-ui PRIVATE PkgConfig::SYSTEMD PkgConfig::WAYLAND)
endif()
# On Windows, copy the Slint DLL next to the application binary so that it's found.
if (WIN32)
    add_custom_command(TARGET scrcpy-ui POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:scrcpy-ui> $<TARGET_FILE_DIR:scrcpy-ui> COMMAND_EXPAND_LISTS)
endif()

if (WIN32)
  configure_file(scripts/update.ps1 app/update.ps1 COPYONLY)
  add_custom_command(TARGET scrcpy-ui POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/update.ps1" "$<TARGET_FILE_DIR:scrcpy-ui>/update.ps1")
endif()
include(CTest)
if(BUILD_TESTING)
  add_executable(scrcpy-core-tests tests/core_tests.cpp)
  target_link_libraries(scrcpy-core-tests PRIVATE scrcpy-core)
  add_test(NAME scrcpy-core COMMAND scrcpy-core-tests)
  add_executable(scrcpy-tool-fixture tests/tool_fixture.cpp)
  target_link_libraries(scrcpy-tool-fixture PRIVATE scrcpy-core)
  add_executable(scrcpy-backend-tests tests/backend_tests.cpp src/scrcpy/Scrcpy.cpp)
  target_link_libraries(scrcpy-backend-tests PRIVATE scrcpy-core)
  add_test(NAME scrcpy-backend COMMAND scrcpy-backend-tests $<TARGET_FILE:scrcpy-tool-fixture>)
  if(WIN32)
    add_executable(scrcpy-file-drop-tests tests/file_drop_tests.cpp)
    target_link_libraries(scrcpy-file-drop-tests PRIVATE scrcpy-core)
    add_test(NAME scrcpy-file-drop COMMAND scrcpy-file-drop-tests)
    add_executable(scrcpy-startup-tests tests/startup_tests.cpp)
    target_link_libraries(scrcpy-startup-tests PRIVATE scrcpy-core)
    add_dependencies(scrcpy-startup-tests scrcpy-ui scrcpy-tool-fixture)
    add_test(NAME scrcpy-startup COMMAND scrcpy-startup-tests $<TARGET_FILE:scrcpy-ui>
      $<TARGET_FILE:scrcpy-tool-fixture> "${CMAKE_BINARY_DIR}/startup-tests")
    set_tests_properties(scrcpy-startup PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 30)
    find_program(POWERSHELL_EXECUTABLE powershell)
  endif()
  if(WIN32 AND POWERSHELL_EXECUTABLE)
    add_test(NAME scrcpy-run-script COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
      -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.ps1"
      -RunScript "${CMAKE_CURRENT_SOURCE_DIR}/run.ps1" -WorkDirectory "${CMAKE_BINARY_DIR}/run-tests")
    add_test(NAME scrcpy-updater COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
      -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/updater_tests.ps1"
      -UpdateScript "${CMAKE_CURRENT_SOURCE_DIR}/scripts/update.ps1"
      -Fixture $<TARGET_FILE:scrcpy-tool-fixture> -WorkDirectory "${CMAKE_BINARY_DIR}/updater-tests")
    find_program(SCRCPY_MAKENSIS_EXECUTABLE makensis
      HINTS "$ENV{ProgramFiles}/NSIS" "$ENV{ProgramFiles\(x86\)}/NSIS"
      PATH_SUFFIXES Bin)
    if(SCRCPY_MAKENSIS_EXECUTABLE)
      add_test(NAME scrcpy-installer COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer_tests.ps1"
        -Template "${CMAKE_CURRENT_SOURCE_DIR}/cmake/NSIS.template.in"
        -MakeNsis "${SCRCPY_MAKENSIS_EXECUTABLE}" -Fixture $<TARGET_FILE:scrcpy-tool-fixture>
        -WorkDirectory "${CMAKE_BINARY_DIR}/installer-tests")
      set_tests_properties(scrcpy-installer PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE TIMEOUT 90)
    endif()
  endif()
  if(SLINT_FEATURE_RENDERER_SOFTWARE)
    add_executable(scrcpy-ui-preview tests/ui_preview.cpp)
    set_target_properties(scrcpy-ui-preview PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    target_link_libraries(scrcpy-ui-preview PRIVATE scrcpy-widgets)
    if(MSVC)
      target_compile_options(scrcpy-ui-preview PRIVATE /utf-8)
    endif()
    if(WIN32)
      add_custom_command(TARGET scrcpy-ui-preview POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_RUNTIME_DLLS:scrcpy-ui-preview> $<TARGET_FILE_DIR:scrcpy-ui-preview> COMMAND_EXPAND_LISTS)
    endif()
    add_test(NAME scrcpy-ui-preview COMMAND scrcpy-ui-preview "${CMAKE_BINARY_DIR}/ui-previews")
  endif()
endif()

if(WIN32)
  install(TARGETS scrcpy-ui RUNTIME DESTINATION . COMPONENT gui)
  install(FILES $<TARGET_RUNTIME_DLLS:scrcpy-ui> DESTINATION . COMPONENT gui)
  install(FILES scripts/update.ps1 README.md LICENSE.txt DESTINATION . COMPONENT gui)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT gui)
  include(InstallRequiredSystemLibraries)
  set(CPACK_GENERATOR ZIP)
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  # Keep the portable ZIP for the updater; `cpack -G NSIS` builds the installer.
  # Our small template uses HKCU consistently, even when run by an administrator.
  set(CPACK_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  set(CPACK_MONOLITHIC_INSTALL ON)
  set(CPACK_PACKAGE_VENDOR "scrcpy-gui")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "scrcpy-gui")
  set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "scrcpy-gui")
  set(CPACK_NSIS_DISPLAY_NAME "scrcpy-gui")
  set(CPACK_NSIS_PACKAGE_NAME "scrcpy-gui")
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt")
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
  set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};scrcpy-ui;gui;/")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
    set(SCRCPY_PACKAGE_ARCH arm64)
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(SCRCPY_PACKAGE_ARCH x64)
  else()
    set(SCRCPY_PACKAGE_ARCH x86)
  endif()
  set(CPACK_PACKAGE_FILE_NAME "scrcpy-gui-windows-${SCRCPY_PACKAGE_ARCH}")
  include(CPack)
else()
  # Everything the app needs is in the "gui" component; Slint's own install
  # rules (headers, compiler, CMake files) stay out of packages:
  #   cmake --install build --component gui --strip
  include(GNUInstallDirs)
  set_target_properties(scrcpy-ui PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
  install(TARGETS scrcpy-ui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT gui)
  # Ship the Slint library with the app: a FetchContent build produces a private
  # libslint_cpp, and a prebuilt SDK (as used by CI) is not a system package.
  if(TARGET slint_cpp-shared)
    set(SCRCPY_SLINT_LIBRARY slint_cpp-shared)
  elseif(TARGET Slint::Slint)
    set(SCRCPY_SLINT_LIBRARY Slint::Slint)
  endif()
  if(SCRCPY_SLINT_LIBRARY)
    install(FILES $<TARGET_FILE:${SCRCPY_SLINT_LIBRARY}> DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT gui)
    install(CODE "if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND \"${CMAKE_STRIP}\" \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/$<TARGET_FILE_NAME:${SCRCPY_SLINT_LIBRARY}>\")
    endif()" COMPONENT gui)
  endif()
  install(FILES packaging/linux/scrcpy-gui.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT gui)
  install(FILES packaging/linux/scrcpy-gui.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps COMPONENT gui)
  install(FILES README.md LICENSE.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/scrcpy-gui COMPONENT gui)
  set(CPACK_GENERATOR TGZ)
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_PACKAGE_VENDOR "scrcpy-gui")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(SCRCPY_PACKAGE_ARCH arm64)
  else()
    set(SCRCPY_PACKAGE_ARCH x64)
  endif()
  set(CPACK_PACKAGE_FILE_NAME "scrcpy-gui-linux-${SCRCPY_PACKAGE_ARCH}")
  set(CPACK_STRIP_FILES ON)
  # The updater installs bin/, lib/ and share/ from a flat archive.
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
  set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};scrcpy-ui;gui;/")
  include(CPack)
endif()
