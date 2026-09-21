set(PACKAGE_DIR "${CMAKE_BINARY_DIR}/package")
file(MAKE_DIRECTORY "${PACKAGE_DIR}")

# Determine configuration for multi- or single-config generators
if(CMAKE_CONFIGURATION_TYPES) # multi-config generator (VS, Xcode)
	set(PACKAGE_CONFIG "$<CONFIG>")
else() # single-config generator (Ninja, Makefiles)
	if(NOT CMAKE_BUILD_TYPE)
		set(PACKAGE_CONFIG "Debug")
	else()
		set(PACKAGE_CONFIG "${CMAKE_BUILD_TYPE}")
	endif()
endif()

# Path to your executable
set(TARGET_EXE "$<TARGET_FILE:NotepadNext>")

# Build the list of arguments for windeployqt
set(WINDEPLOYQT_ARGS --no-translations --no-system-d3d-compiler --no-compiler-runtime --no-opengl-sw)
if(PACKAGE_CONFIG STREQUAL "Debug")
	list(APPEND WINDEPLOYQT_ARGS --debug)
endif()
list(APPEND WINDEPLOYQT_ARGS "${PACKAGE_DIR}/NotepadNext.exe")

file(GLOB EXTRA_DLLS "${EXTRA_DLL_DIR}/*.dll")

# Define the package target
add_custom_target(package
	COMMENT "Packaging NotepadNext for distribution"
	VERBATIM

	# Copy executable
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${TARGET_EXE}"
		"${PACKAGE_DIR}/NotepadNext.exe"

	# Copy LICENSE
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${CMAKE_SOURCE_DIR}/LICENSE"
		"${PACKAGE_DIR}/LICENSE"

	# Copy the two extra DLLs
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${CMAKE_SOURCE_DIR}/deploy/windows/libcrypto-1_1-x64.dll"
		"${PACKAGE_DIR}/libcrypto-1_1-x64.dll"

	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${CMAKE_SOURCE_DIR}/deploy/windows/libssl-1_1-x64.dll"
		"${PACKAGE_DIR}/libssl-1_1-x64.dll"

	# Run windeployqt with correct flags
	COMMAND windeployqt ${WINDEPLOYQT_ARGS}
)

# Bundled ctags and cscope (Windows x86_64 only). The binaries are copied
# flat into the package root so that the application's own lookup (next to
# NotepadNext.exe) finds them without any PATH configuration, mirroring the
# deploy/macos/ctags_x64 + cscope_x64 bundles of the macOS packaging.
#
# The MinGW runtime DLLs are bundled as well: windeployqt runs with
# --no-compiler-runtime, and without them the executable cannot start on a
# machine that does not have the compiler's bin directory on its PATH.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	get_filename_component(MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
	set(MINGW_RUNTIME_DLLS
		libgcc_s_seh-1.dll
		libstdc++-6.dll
		libwinpthread-1.dll
	)
	set(MINGW_RUNTIME_COMMANDS "")
	foreach(dll ${MINGW_RUNTIME_DLLS})
		if(EXISTS "${MINGW_BIN_DIR}/${dll}")
			list(APPEND MINGW_RUNTIME_COMMANDS
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
					"${MINGW_BIN_DIR}/${dll}"
					"${PACKAGE_DIR}/${dll}"
			)
		endif()
	endforeach()

	add_custom_command(TARGET package POST_BUILD
		COMMENT "Bundling ctags and cscope (windows x86_64)"
		VERBATIM

		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"${CMAKE_SOURCE_DIR}/deploy/windows/ctags_x86_64"
			"${PACKAGE_DIR}/"

		COMMAND ${CMAKE_COMMAND} -E copy_directory
			"${CMAKE_SOURCE_DIR}/deploy/windows/cscope_x86_64"
			"${PACKAGE_DIR}/"

		${MINGW_RUNTIME_COMMANDS}
	)
endif()

set(ZIP_FILE "${CMAKE_BINARY_DIR}/NotepadNext-v${PROJECT_VERSION}.zip")

# Locate 7-Zip: normally on PATH, otherwise fall back to the default install
# directory on Windows.
find_program(SEVENZ_EXECUTABLE 7z
	PATHS "C:/Program Files/7-Zip" "C:/Program Files (x86)/7-Zip"
)
if(NOT SEVENZ_EXECUTABLE)
	message(FATAL_ERROR "7z.exe not found - install 7-Zip or add it to PATH to build the portable zip")
endif()

add_custom_target(zip
	DEPENDS package
	COMMENT "Creating zip archive of NotepadNext package"
	VERBATIM
	# 7z "a" updates an existing archive instead of rebuilding it, so a stale
	# zip from an earlier layout would silently survive inside the new one.
	# Always start from a clean file.
	COMMAND ${CMAKE_COMMAND} -E rm -f "${ZIP_FILE}"
	COMMAND "${SEVENZ_EXECUTABLE}" a -tzip
		"${ZIP_FILE}"
		"${PACKAGE_DIR}/*"
		-x!libcrypto-1_1-x64.dll
		-x!libssl-1_1-x64.dll
)

set(NSIS_SCRIPT "${CMAKE_SOURCE_DIR}/installer/installer.nsi")
add_custom_target(installer
	DEPENDS package zip
	COMMENT "Building NSIS installer for NotepadNext (portable zip is built as well)"
	VERBATIM
	COMMAND makensis /V4 "${NSIS_SCRIPT}"
)
