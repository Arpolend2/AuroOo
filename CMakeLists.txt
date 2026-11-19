cmake_minimum_required(VERSION 3.5)
project(ru.arpolend.AuroOo VERSION 1.3 LANGUAGES CXX)

find_package(Qt5 COMPONENTS Core Gui Network Qml Quick DBus REQUIRED)
find_package(Qt5LinguistTools REQUIRED)
qt5_add_translation(TRANSLATION_FILES translations/nextaurocloud_ru.ts)
add_custom_target(translations ALL DEPENDS ${TRANSLATION_FILES})

include(FindPkgConfig)
pkg_search_module(AURORA auroraapp REQUIRED)

set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(${PROJECT_NAME}
    CMakeLists.txt
    src/main.cpp
    src/nextcloudclient.cpp
    src/nextcloudclient.h
    src/tokenstore.cpp
    src/tokenstore.h
    src/webdavparser.cpp
    src/webdavparser.h
    qml/pages/FileBrowserPanel.qml
    qml/pages/WheelScroll.qml
    qml/pages/UploadPicker.qml
    qml/icons/cover-upload.svg
    qml/covers/CoverPage.qml
    ${PROJECT_NAME}.desktop
    rpm/${PROJECT_NAME}.spec
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${AURORA_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME}
    Qt5::Core
    Qt5::Gui
    Qt5::Network
    Qt5::Qml
    Qt5::Quick
    Qt5::DBus
    ${AURORA_LDFLAGS}
    runtime-manager-qt5
)

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)
install(DIRECTORY qml
    DESTINATION share/${PROJECT_NAME}
)
install(FILES ${TRANSLATION_FILES}
    DESTINATION share/${PROJECT_NAME}/translations
)
install(FILES LICENSES/UIComponentGallery-BSD-3-Clause.txt
    DESTINATION share/${PROJECT_NAME}/licenses
)
install(FILES ${PROJECT_NAME}.desktop
    DESTINATION share/applications
)
install(DIRECTORY icons/hicolor
    DESTINATION share/icons
)

file(WRITE "${CMAKE_BINARY_DIR}/QtCreatorDeployment.txt"
    "${CMAKE_INSTALL_PREFIX}\n${CMAKE_BINARY_DIR}/${PROJECT_NAME}:bin\n"
)

add_custom_target(distfiles SOURCES
    README.md
    LICENSES/UIComponentGallery-BSD-3-Clause.txt
    translations/nextaurocloud_ru.ts
    CMakeLists.txt
    ${PROJECT_NAME}.desktop
    rpm/${PROJECT_NAME}.spec
    icons/hicolor/86x86/apps/${PROJECT_NAME}.png
    icons/hicolor/108x108/apps/${PROJECT_NAME}.png
    icons/hicolor/128x128/apps/${PROJECT_NAME}.png
    icons/hicolor/172x172/apps/${PROJECT_NAME}.png
    icons/cloud-files.png
    qml/NextcloudFiles.qml
    qml/pages/LoginPage.qml
    qml/pages/FilesPage.qml
    qml/pages/FileBrowserPanel.qml
    qml/pages/WheelScroll.qml
    qml/pages/UploadPicker.qml
    qml/icons/cover-upload.svg
    qml/covers/CoverPage.qml
    src/webdavparser.cpp
    src/webdavparser.h
    tests/webdavparser_tests.cpp
    tests/securestorage_tests.cpp
    tests/qmlwheel_tests.cpp
    tests/tokenlogin_tests.cpp
    tests/package_identity_tests.cmake
    tests/qml/wheelfixture.qml
    src/tokenstore.cpp
    src/tokenstore.h
)

option(BUILD_PACKAGING_TESTS "Check package identity consistency" OFF)
if(BUILD_PACKAGING_TESTS)
    enable_testing()
    add_test(NAME package_identity_tests
        COMMAND ${CMAKE_COMMAND}
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DBINARY_DIR=${CMAKE_BINARY_DIR}"
            "-DPACKAGE_ID=${PROJECT_NAME}"
            "-DPACKAGE_VERSION=${PROJECT_VERSION}"
            -P "${CMAKE_SOURCE_DIR}/tests/package_identity_tests.cmake")
endif()

option(BUILD_WEBDAV_PARSER_TESTS "Build focused WebDAV parser tests" OFF)
if(BUILD_WEBDAV_PARSER_TESTS)
    find_package(Qt5 COMPONENTS Test REQUIRED)
    enable_testing()
    add_executable(webdavparser_tests
        src/webdavparser.cpp
        src/webdavparser.h
        tests/webdavparser_tests.cpp
    )
    target_include_directories(webdavparser_tests PRIVATE src)
    target_link_libraries(webdavparser_tests Qt5::Core Qt5::Test)
    add_test(NAME webdavparser_tests COMMAND webdavparser_tests)
endif()

option(BUILD_TOKEN_LOGIN_TESTS "Build token login regression tests" OFF)
if(BUILD_TOKEN_LOGIN_TESTS)
    find_package(Qt5 COMPONENTS Test REQUIRED)
    enable_testing()
    add_executable(tokenlogin_tests
        src/nextcloudclient.cpp src/nextcloudclient.h
        src/tokenstore.cpp src/tokenstore.h
        src/webdavparser.cpp src/webdavparser.h
        tests/tokenlogin_tests.cpp
    )
    target_include_directories(tokenlogin_tests PRIVATE src ${AURORA_INCLUDE_DIRS})
    target_link_libraries(tokenlogin_tests
        Qt5::Core Qt5::Gui Qt5::Network Qt5::Qml Qt5::Quick Qt5::DBus Qt5::Test
        ${AURORA_LDFLAGS} runtime-manager-qt5
    )
    add_test(NAME tokenlogin_tests COMMAND tokenlogin_tests)
endif()

option(BUILD_QML_WHEEL_TESTS "Build QML wheel regression tests" OFF)
if(BUILD_QML_WHEEL_TESTS)
    find_package(Qt5 COMPONENTS Test REQUIRED)
    enable_testing()
    add_executable(qmlwheel_tests tests/qmlwheel_tests.cpp)
    target_compile_definitions(qmlwheel_tests PRIVATE
        WHEEL_FIXTURE_PATH="${CMAKE_SOURCE_DIR}/tests/qml/wheelfixture.qml"
        RUSSIAN_CATALOG_PATH="${CMAKE_CURRENT_BINARY_DIR}/nextaurocloud_ru.qm")
    add_dependencies(qmlwheel_tests translations)
    target_link_libraries(qmlwheel_tests Qt5::Core Qt5::Gui Qt5::Qml Qt5::Quick Qt5::Test)
    add_test(NAME qmlwheel_tests COMMAND qmlwheel_tests)

    # Keep native SplitView navigation in its own headless Qt process.
    add_executable(qmlnavigation_tests tests/qmlwheel_tests.cpp)
    target_compile_definitions(qmlnavigation_tests PRIVATE
        NAVIGATION_TESTS_ONLY
        WHEEL_FIXTURE_PATH="${CMAKE_SOURCE_DIR}/tests/qml/wheelfixture.qml"
        RUSSIAN_CATALOG_PATH="${CMAKE_CURRENT_BINARY_DIR}/nextaurocloud_ru.qm")
    target_link_libraries(qmlnavigation_tests Qt5::Core Qt5::Gui Qt5::Qml Qt5::Quick Qt5::Test)
    add_test(NAME qmlnavigation_tests COMMAND qmlnavigation_tests)
endif()

option(BUILD_SECURE_STORAGE_TESTS "Build secure-storage login regression tests" OFF)
if(BUILD_SECURE_STORAGE_TESTS)
    find_package(Qt5 COMPONENTS Test REQUIRED)
    enable_testing()
    add_executable(securestorage_tests
        src/nextcloudclient.cpp
        src/nextcloudclient.h
        src/tokenstore.cpp
        src/tokenstore.h
        src/webdavparser.cpp
        src/webdavparser.h
        tests/securestorage_tests.cpp
    )
    target_include_directories(securestorage_tests PRIVATE src ${AURORA_INCLUDE_DIRS})
    target_link_libraries(securestorage_tests
        Qt5::Core Qt5::Gui Qt5::Network Qt5::Qml Qt5::Quick Qt5::DBus Qt5::Test
        ${AURORA_LDFLAGS} runtime-manager-qt5
    )
    add_test(NAME securestorage_tests COMMAND securestorage_tests)
endif()
