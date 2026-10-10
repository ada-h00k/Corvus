cmake_minimum_required(VERSION 3.16)
project(signal-cli-qt VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(Threads REQUIRED)

# ---- Rust: encrypted history store (static library with C ABI) ------------
find_program(CARGO cargo REQUIRED)
set(STORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/history-store)
set(CARGO_TARGET_DIR ${CMAKE_BINARY_DIR}/cargo)
set(STORE_LIB ${CARGO_TARGET_DIR}/release/${CMAKE_STATIC_LIBRARY_PREFIX}history_store${CMAKE_STATIC_LIBRARY_SUFFIX})

add_custom_target(history_store_cargo
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${CARGO_TARGET_DIR}
            ${CARGO} build --release --locked --manifest-path ${STORE_DIR}/Cargo.toml
    BYPRODUCTS ${STORE_LIB}
    COMMENT "Building Rust history store"
    USES_TERMINAL)

add_library(history_store STATIC IMPORTED GLOBAL)
set_target_properties(history_store PROPERTIES
    IMPORTED_LOCATION ${STORE_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${STORE_DIR}/include)
add_dependencies(history_store history_store_cargo)

# ---- Qt GUI ---------------------------------------------------------------
set(APP_SOURCES
    src/Model.h
    src/Appearance.h
    src/Appearance.cpp
    src/AppearanceDialog.h
    src/AppearanceDialog.cpp
    src/Attachments.h
    src/Attachments.cpp
    src/Records.h
    src/Records.cpp
    src/Envelope.h
    src/Envelope.cpp
    src/EmojiPicker.h
    src/EmojiPicker.cpp
    src/ImageViewer.h
    src/ImageViewer.cpp
    src/ChatView.h
    src/ChatView.cpp
    src/SignalCli.h
    src/SignalCli.cpp
    src/HistoryStore.h
    src/HistoryStore.cpp
    src/PassphraseDialog.h
    src/PassphraseDialog.cpp
    src/MessageInput.h
    src/MessageInput.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/LinkDialog.h
    src/LinkDialog.cpp
)
add_library(app_core STATIC ${APP_SOURCES})
target_include_directories(app_core PUBLIC src)
target_compile_definitions(app_core PUBLIC APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(app_core PUBLIC
    Qt6::Widgets Qt6::Network history_store Threads::Threads ${CMAKE_DL_LIBS} m)

add_executable(signal-cli-qt src/main.cpp)
target_link_libraries(signal-cli-qt PRIVATE app_core)

# ---- Tests (headless): cmake -DSIGNAL_CLI_QT_TESTS=ON, then ctest ---------
option(SIGNAL_CLI_QT_TESTS "Build the GUI integration test" OFF)
if(SIGNAL_CLI_QT_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    # copy the mock with the executable bit set (independent of the source tree's file modes)
    file(COPY tests/mock-signal-cli.py DESTINATION ${CMAKE_BINARY_DIR}/tests
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    add_executable(gui_integration_test tests/gui_integration_test.cpp)
    target_link_libraries(gui_integration_test PRIVATE app_core Qt6::Test)
    target_compile_definitions(gui_integration_test PRIVATE
        MOCK_CLI="${CMAKE_BINARY_DIR}/tests/mock-signal-cli.py")
    add_test(NAME gui_integration COMMAND gui_integration_test)

    add_executable(startup_test tests/startup_test.cpp)
    target_link_libraries(startup_test PRIVATE app_core)
    target_compile_definitions(startup_test PRIVATE
        MOCK_CLI="${CMAKE_BINARY_DIR}/tests/mock-signal-cli.py")
    add_test(NAME startup COMMAND startup_test)
    set_tests_properties(startup PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 240)

    add_executable(chatview_test tests/chatview_test.cpp)
    target_link_libraries(chatview_test PRIVATE app_core Qt6::Test)
    add_test(NAME chatview COMMAND chatview_test)
    set_tests_properties(chatview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    set_tests_properties(gui_integration PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 240)
endif()

include(GNUInstallDirs)
install(TARGETS signal-cli-qt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/signal-cli-qt.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/signal-cli-qt.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
