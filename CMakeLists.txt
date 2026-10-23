cmake_minimum_required(VERSION 3.16)
project(corvus VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Multimedia MultimediaWidgets Svg)
find_package(Qt6Keychain 0.14 REQUIRED)
find_package(Threads REQUIRED)

# Desktop notifications use the freedesktop.org D-Bus service (Linux/BSD).
if(UNIX AND NOT APPLE)
    find_package(Qt6 QUIET OPTIONAL_COMPONENTS DBus)
endif()

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
    USES_TERMINAL
)

add_library(history_store STATIC IMPORTED GLOBAL)
set_target_properties(history_store PROPERTIES
    IMPORTED_LOCATION ${STORE_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${STORE_DIR}/include
)
add_dependencies(history_store history_store_cargo)

# ---- Qt GUI ---------------------------------------------------------------
set(APP_SOURCES
    resources/sounds.qrc
    resources/icons.qrc

    src/Model.h
    src/NotificationSettings.h
    src/NotificationSettings.cpp
    src/SoundPlayer.h
    src/SoundPlayer.cpp
    src/Notifier.h
    src/Notifier.cpp
    src/NotificationDialog.h
    src/NotificationDialog.cpp
    src/AppIcon.h
    src/AppIcon.cpp
    src/Appearance.h
    src/Appearance.cpp
    src/AppearanceDialog.h
    src/AppearanceDialog.cpp
    src/ProfileDialog.h
    src/ProfileDialog.cpp
    src/Attachments.h
    src/Attachments.cpp
    src/Records.h
    src/Records.cpp
    src/Envelope.h
    src/Envelope.cpp
    src/EmojiPicker.h
    src/EmojiPicker.cpp
    src/GiphyPicker.h
    src/GiphyPicker.cpp
    src/GiphyApiKeyStore.h
    src/GiphyApiKeyStore.cpp
    src/ChatView.h
    src/ChatView.cpp
    src/ImageViewer.h
    src/ImageViewer.cpp
    src/MediaGallery.h
    src/MediaGallery.cpp
    src/SignalCli.h
    src/SignalCli.cpp
    src/HistoryStore.h
    src/HistoryStore.cpp
    src/HistoryKeychain.h
    src/HistoryKeychain.cpp
    src/PassphraseDialog.h
    src/PassphraseDialog.cpp
    src/MessageInput.h
    src/MessageInput.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/LinkDialog.h
    src/LinkDialog.cpp
    src/LegacyMigration.h
    src/LegacyMigration.cpp
)

add_library(app_core STATIC ${APP_SOURCES})
target_include_directories(app_core PUBLIC src)
target_compile_definitions(app_core PUBLIC APP_VERSION="${PROJECT_VERSION}")

target_link_libraries(app_core PUBLIC
    Qt6::Widgets
    Qt6::Network
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::Svg
    Qt6Keychain::Qt6Keychain
    history_store
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    # System libraries required by the Rust static library (std + OS RNG, see history-store/src/rng.rs).
    target_link_libraries(app_core PUBLIC
        advapi32 bcrypt kernel32 ntdll userenv ws2_32 synchronization)
    # M_PI etc. in <cmath>; keep <windows.h> from defining min/max macros.
    target_compile_definitions(app_core PUBLIC _USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        # The sources contain UTF-8 literals (…, ⏎, emoji).
        target_compile_options(app_core PUBLIC /utf-8)
    endif()
else()
    target_link_libraries(app_core PUBLIC m)
endif()

if(TARGET Qt6::DBus)
    target_link_libraries(app_core PUBLIC Qt6::DBus)
    target_compile_definitions(app_core PUBLIC HAVE_QT_DBUS)
    message(STATUS "Desktop notifications: freedesktop.org D-Bus")
else()
    message(STATUS "Desktop notifications: tray icon fallback only (Qt DBus not found)")
endif()

# ---- Application executable -----------------------------------------------
if(WIN32)
    # GUI subsystem (no console window) with application icon and version info.
    configure_file(packaging/windows/corvus.rc.in ${CMAKE_BINARY_DIR}/corvus.rc @ONLY)
    add_executable(corvus WIN32 src/main.cpp ${CMAKE_BINARY_DIR}/corvus.rc)
else()
    add_executable(corvus src/main.cpp)
endif()
target_link_libraries(corvus PRIVATE app_core)

# Build only the main Qt application as a macOS .app bundle.
# Linux and other platforms continue to build a normal executable.
if(APPLE)
    # Own Info.plist: sets LSFileQuarantineEnabled, so files created by the app
    # (received attachments, temporary copies) get the com.apple.quarantine flag.
    # Child processes inherit that, hence the signal-cli helper app below.
    configure_file(packaging/macos/Info.plist.in
        ${CMAKE_BINARY_DIR}/Info.plist @ONLY)
    set_target_properties(corvus PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/Info.plist
    )

    # signal-cli helper, embedded as Contents/Helpers/corvus-signal-cli.app (src/SignalCliHelper.cpp).
    configure_file(packaging/macos/SignalCliHelper.plist.in
        ${CMAKE_BINARY_DIR}/SignalCliHelper.plist @ONLY)
    add_executable(corvus-signal-cli MACOSX_BUNDLE src/SignalCliHelper.cpp)
    set_target_properties(corvus-signal-cli PROPERTIES
        AUTOMOC OFF
        AUTORCC OFF
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/SignalCliHelper.plist
    )
    # Whenever either bundle is rebuilt; the outer bundle is signed again around the new helper.
    set(CORVUS_HELPER_APP "$<TARGET_BUNDLE_CONTENT_DIR:corvus>/Helpers/corvus-signal-cli.app")
    set(CORVUS_HELPER_STAMP ${CMAKE_BINARY_DIR}/corvus-signal-cli-embedded.stamp)
    add_custom_command(OUTPUT ${CORVUS_HELPER_STAMP}
        COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_BUNDLE_DIR:corvus-signal-cli>" "${CORVUS_HELPER_APP}"
        COMMAND codesign --force --sign - "${CORVUS_HELPER_APP}"
        COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:corvus>"
        COMMAND ${CMAKE_COMMAND} -E touch ${CORVUS_HELPER_STAMP}
        DEPENDS corvus corvus-signal-cli
        VERBATIM
    )
    add_custom_target(corvus-signal-cli-embed ALL DEPENDS ${CORVUS_HELPER_STAMP})
endif()

# ---- Tests (headless): cmake -DCORVUS_TESTS=ON, then ctest ------------------
option(CORVUS_TESTS "Build the GUI integration test" OFF)

if(CORVUS_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)

    file(COPY tests/mock-signal-cli.py DESTINATION ${CMAKE_BINARY_DIR}/tests
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                         GROUP_READ GROUP_EXECUTE
                         WORLD_READ WORLD_EXECUTE)

    add_executable(gui_integration_test tests/gui_integration_test.cpp)
    target_link_libraries(gui_integration_test PRIVATE app_core Qt6::Test)
    target_compile_definitions(gui_integration_test PRIVATE
        MOCK_CLI="${CMAKE_BINARY_DIR}/tests/mock-signal-cli.py")
    add_test(NAME gui_integration COMMAND gui_integration_test)

    add_executable(migration_test tests/migration_test.cpp)
    target_link_libraries(migration_test PRIVATE app_core Qt6::Test)
    add_test(NAME migration COMMAND migration_test)
    set_tests_properties(migration PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60
    )

    add_executable(startup_test tests/startup_test.cpp)
    target_link_libraries(startup_test PRIVATE app_core)
    target_compile_definitions(startup_test PRIVATE
        MOCK_CLI="${CMAKE_BINARY_DIR}/tests/mock-signal-cli.py")
    add_test(NAME startup COMMAND startup_test)
    set_tests_properties(startup PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 240
    )

    if(TARGET Qt6::DBus)
        find_program(DBUS_RUN_SESSION dbus-run-session)
        if(DBUS_RUN_SESSION)
            add_executable(alerts_test tests/alerts_test.cpp)
            target_link_libraries(alerts_test PRIVATE app_core Qt6::DBus Qt6::Test)
            target_compile_definitions(alerts_test PRIVATE
                MOCK_CLI="${CMAKE_BINARY_DIR}/tests/mock-signal-cli.py")

            find_program(XVFB_RUN xvfb-run)
            if(XVFB_RUN)
                add_test(NAME alerts
                    COMMAND ${XVFB_RUN} -a ${DBUS_RUN_SESSION} -- $<TARGET_FILE:alerts_test>)
                set_tests_properties(alerts PROPERTIES
                    ENVIRONMENT "QT_QPA_PLATFORM=xcb"
                    TIMEOUT 300
                )
            else()
                add_test(NAME alerts
                    COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:alerts_test>)
                set_tests_properties(alerts PROPERTIES
                    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                    TIMEOUT 300
                )
            endif()
        else()
            message(STATUS "dbus-run-session not found: alerts test skipped")
        endif()
    endif()

    add_executable(media_gallery_test tests/media_gallery_test.cpp)
    target_link_libraries(media_gallery_test PRIVATE app_core Qt6::Test)
    add_test(NAME media_gallery COMMAND media_gallery_test)
    set_tests_properties(media_gallery PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60
    )

    add_executable(emoji_gif_test tests/emoji_gif_test.cpp)
    target_link_libraries(emoji_gif_test PRIVATE app_core Qt6::Test)
    add_test(NAME emoji_gif COMMAND emoji_gif_test)
    set_tests_properties(emoji_gif PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60
    )

    add_executable(conversation_order_test tests/conversation_order_test.cpp)
    target_link_libraries(conversation_order_test PRIVATE app_core Qt6::Test)
    add_test(NAME conversation_order COMMAND conversation_order_test)
    set_tests_properties(conversation_order PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 60
    )

    add_executable(chatview_test tests/chatview_test.cpp)
    target_link_libraries(chatview_test PRIVATE app_core Qt6::Test)
    add_test(NAME chatview COMMAND chatview_test)
    set_tests_properties(chatview PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 120
    )

    set_tests_properties(gui_integration PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 240
    )
endif()

# ---- Installation ---------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS corvus
    # Required when corvus is built as a MACOSX_BUNDLE.
    BUNDLE DESTINATION .
    # Used for regular executables on Linux and other platforms.
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(NOT WIN32)
    install(FILES packaging/corvus.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    )

    install(FILES packaging/corvus.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
    )
endif()
# Screenshot tool (only built on request: cmake --build build --target shots)
add_executable(shots EXCLUDE_FROM_ALL tests/shots.cpp)
target_link_libraries(shots PRIVATE app_core)
