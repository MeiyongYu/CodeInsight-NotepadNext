message(STATUS "Configuring macOS packaging")

set(INSTALL_DIR ${CMAKE_BINARY_DIR}/install)

# Custom Info.plist
set_target_properties(NotepadNext PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/deploy/macos/info.plist
)

# Application icon
set(APP_ICON_MACOS ${CMAKE_SOURCE_DIR}/icon/NotepadNext.icns)

set_source_files_properties(${APP_ICON_MACOS}
    PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
)

target_sources(NotepadNext PRIVATE ${APP_ICON_MACOS})

set_target_properties(NotepadNext PROPERTIES
    MACOSX_BUNDLE_ICON_FILE NotepadNext.icns
)

install(TARGETS NotepadNext
    BUNDLE DESTINATION .
)

install(FILES ${APP_ICON_MACOS}
    DESTINATION NotepadNext.app/Contents/Resources
)

# Bundled ctags and cscope binaries (macOS x86_64 only)
if(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64" OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"))
    set(CTAGS_BINARY ${CMAKE_SOURCE_DIR}/deploy/macos/ctags_x64/ctags)
    set(CSCOPE_BINARY ${CMAKE_SOURCE_DIR}/deploy/macos/cscope_x64/cscope)

    set_source_files_properties(${CTAGS_BINARY}
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
    )
    set_source_files_properties(${CSCOPE_BINARY}
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
    )

    target_sources(NotepadNext PRIVATE ${CTAGS_BINARY} ${CSCOPE_BINARY})

    install(PROGRAMS ${CTAGS_BINARY} ${CSCOPE_BINARY}
        DESTINATION NotepadNext.app/Contents/Resources
    )
endif()

add_custom_target(install_local
    COMMAND ${CMAKE_COMMAND}
        --install ${CMAKE_BINARY_DIR}
        --prefix ${INSTALL_DIR}
    DEPENDS NotepadNext
)

find_program(MACDEPLOYQT_EXECUTABLE macdeployqt REQUIRED)

set(DMG_STAGING_DIR ${CMAKE_BINARY_DIR}/dmg-staging)

add_custom_target(dmg
    # Create staging directory with app and Applications symlink
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${DMG_STAGING_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DMG_STAGING_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${INSTALL_DIR}/NotepadNext.app
        ${DMG_STAGING_DIR}/NotepadNext.app
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        /Applications
        ${DMG_STAGING_DIR}/Applications
    # Run macdeployqt on the staging directory to deploy Qt frameworks
    COMMAND ${MACDEPLOYQT_EXECUTABLE}
        ${DMG_STAGING_DIR}/NotepadNext.app
    # Create DMG from the staging directory
    COMMAND hdiutil create
        -volname "NotepadNext ${PROJECT_VERSION}"
        -srcfolder ${DMG_STAGING_DIR}
        -ov -format UDZO
        ${CMAKE_BINARY_DIR}/NotepadNext-v${PROJECT_VERSION}.dmg
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${DMG_STAGING_DIR}
    DEPENDS install_local
)
