# PS4 packaging: engine ELF -> eboot.bin (fself) -> .pkg (fpkg).
#
# We deliberately do NOT use the toolchain's add_self()/add_pkg() helpers: their
# default fself paid (0x38...0035 + full authinfo) launches the app with restrictive
# settings. Instead we mirror the known-good OpenOrbis sample:
#   - create-fself --paid 0x3800000000000011 (no authinfo)
#   - param.sfo CATEGORY "gd" (full game application, APP_TYPE 1)
#
# IMPORTANT (see CLAUDE.md): the pkg bundles ONLY the engine + sce_sys +
# sce_module assets. No game data (.dat/data/sound) is ever packaged.

set(PS4_TITLE       "Fallout II Community Edition")
set(PS4_TITLE_ID    "FALL20002")
set(PS4_VERSION     "01.00")
set(PS4_CONTENT_ID  "IV0001-${PS4_TITLE_ID}_00-FALL200020100000")
set(PS4_PAID        "0x3800000000000011")

set(PS4_PKG_DIR "${CMAKE_CURRENT_BINARY_DIR}/pkg")
set(PS4_SCE_SYS_SRC "${CMAKE_SOURCE_DIR}/os/ps4/sce_sys")
set(PS4_SCE_MODULE_SRC "${CMAKE_SOURCE_DIR}/os/ps4/sce_module")

# Optional custom app icon — keeps the repo free of any non-free artwork. Drop a
# 512x512 PNG named icon0.png at the build root, or pass -DPS4_ICON=/path/to.png;
# if none is present the committed placeholder os/ps4/sce_sys/icon0.png is used.
# (The root icon0.png is gitignored; the overlay build's scripts/apply.sh copies a
#  repo-root icon0.png into the reconstructed tree so this picks it up. Re-run cmake
#  after first dropping the file — EXISTS is checked at configure time.)
set(PS4_ICON "${CMAKE_SOURCE_DIR}/icon0.png" CACHE FILEPATH "Custom 512x512 PS4 app icon PNG (overrides the placeholder)")

# Staged assets (icon0.png, sce_module) are copied into the pkg but aren't
# build outputs, so DEPENDS on them — otherwise editing/dropping one won't repackage.
file(GLOB PS4_PKG_ASSETS "${PS4_SCE_SYS_SRC}/*" "${PS4_SCE_MODULE_SRC}/*")
if(EXISTS "${PS4_ICON}")
    message(STATUS "PS4: using custom app icon ${PS4_ICON}")
    set(PS4_ICON_OVERRIDE COMMAND ${CMAKE_COMMAND} -E copy "${PS4_ICON}" "${PS4_PKG_DIR}/sce_sys/icon0.png")
    list(APPEND PS4_PKG_ASSETS "${PS4_ICON}")
else()
    set(PS4_ICON_OVERRIDE "")
endif()

set(OPENORBIS "$ENV{OPENORBIS}")
set(PKGTOOL "${OPENORBIS}/bin/create-fself")
set(SFO_TOOL "${OPENORBIS}/bin/linux/PkgTool.Core")
set(GP4_TOOL "${OPENORBIS}/bin/linux/create-gp4")
set(DOTFIX "DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1")

# 1. ELF -> eboot.bin (fake-self), signed like the SDL sample (paid 0x...0011).
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/eboot.bin"
    COMMAND ${CMAKE_COMMAND} -E env "OO_PS4_TOOLCHAIN=${OPENORBIS}"
            "${PKGTOOL}" "-in=$<TARGET_FILE:${EXECUTABLE_NAME}>"
            "-out=${CMAKE_BINARY_DIR}/${EXECUTABLE_NAME}.oelf"
            "--eboot" "${CMAKE_BINARY_DIR}/eboot.bin"
            "--paid" "${PS4_PAID}"
    DEPENDS ${EXECUTABLE_NAME}
    COMMENT "Creating eboot.bin (fake-self, paid ${PS4_PAID})"
    VERBATIM)

# 2. param.sfo with CATEGORY "gd" (full game application).
add_custom_command(
    OUTPUT "${PS4_PKG_DIR}/sce_sys/param.sfo"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PS4_PKG_DIR}/sce_sys"
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${SFO_TOOL} sfo_new "${PS4_PKG_DIR}/sce_sys/param.sfo"
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${SFO_TOOL} sfo_setentry "${PS4_PKG_DIR}/sce_sys/param.sfo" APP_TYPE --type Integer --maxsize 4 --value 1
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${SFO_TOOL} sfo_setentry "${PS4_PKG_DIR}/sce_sys/param.sfo" APP_VER --type Utf8 --maxsize 8 --value "${PS4_VERSION}"
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${SFO_TOOL} sfo_setentry "${PS4_PKG_DIR}/sce_sys/param.sfo" ATTRIBUTE --type Integer --maxsize 4 --value 0
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${SFO_TOOL} sfo_setentry "${PS4_PKG_DIR}/sce_sys/param.sfo" CATEGORY --type Utf8 --maxsize 4 --value "gd"
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${SFO_TOOL} sfo_setentry "${PS4_PKG_DIR}/sce_sys/param.sfo" CONTENT_ID --type Utf8 --maxsize 48 --value "${PS4_CONTENT_ID}"
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${SFO_TOOL} sfo_setentry "${PS4_PKG_DIR}/sce_sys/param.sfo" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${SFO_TOOL} sfo_setentry "${PS4_PKG_DIR}/sce_sys/param.sfo" SYSTEM_VER --type Integer --maxsize 4 --value 0
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${SFO_TOOL} sfo_setentry "${PS4_PKG_DIR}/sce_sys/param.sfo" TITLE --type Utf8 --maxsize 128 --value "${PS4_TITLE}"
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${SFO_TOOL} sfo_setentry "${PS4_PKG_DIR}/sce_sys/param.sfo" TITLE_ID --type Utf8 --maxsize 12 --value "${PS4_TITLE_ID}"
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${SFO_TOOL} sfo_setentry "${PS4_PKG_DIR}/sce_sys/param.sfo" VERSION --type Utf8 --maxsize 8 --value "${PS4_VERSION}"
    DEPENDS ${EXECUTABLE_NAME}
    COMMENT "Generating param.sfo (CATEGORY gd)"
    VERBATIM)

# 3. eboot.bin + staged pkg dir -> .pkg
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/${PS4_CONTENT_ID}.pkg"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PS4_PKG_DIR}/sce_sys"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PS4_SCE_SYS_SRC}" "${PS4_PKG_DIR}/sce_sys"
    ${PS4_ICON_OVERRIDE}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PS4_PKG_DIR}/sce_module"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PS4_SCE_MODULE_SRC}" "${PS4_PKG_DIR}/sce_module"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/eboot.bin" "${PS4_PKG_DIR}/eboot.bin"
    COMMAND "${GP4_TOOL}" -out "${PS4_PKG_DIR}/${EXECUTABLE_NAME}.gp4" --content-id "${PS4_CONTENT_ID}" --path "${PS4_PKG_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env ${DOTFIX} ${SFO_TOOL} pkg_build "${PS4_PKG_DIR}/${EXECUTABLE_NAME}.gp4" "${CMAKE_BINARY_DIR}"
    DEPENDS "${CMAKE_BINARY_DIR}/eboot.bin" "${PS4_PKG_DIR}/sce_sys/param.sfo" ${PS4_PKG_ASSETS}
    COMMENT "Building ${PS4_CONTENT_ID}.pkg"
    VERBATIM)

add_custom_target(${EXECUTABLE_NAME}_pkg ALL
    DEPENDS "${CMAKE_BINARY_DIR}/${PS4_CONTENT_ID}.pkg")
