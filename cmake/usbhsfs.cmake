# USB drives for the Switch installer (platform/switch/installer.cpp): libusbhsfs (ISC) at a pinned revision, with a USB
# Attached SCSI transport, FatFs fixes and its USB request enums left to the libnx releases that declare them
# (platform/switch/libusbhsfs.patch). FAT and exFAT only: the NTFS and EXT4 backends would bring their GPL into
# SafeHouse.nro.
include(FetchContent)
set(USBHSFS_GPL OFF CACHE BOOL "" FORCE)
set(USBHSFS_NTFS OFF CACHE BOOL "" FORCE)
set(USBHSFS_EXT4 OFF CACHE BOOL "" FORCE)
set(USBHSFS_SXOS_DISABLE ON CACHE BOOL "" FORCE)
set(USBHSFS_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libusbhsfs
  GIT_REPOSITORY https://github.com/ITotalJustice/libusbhsfs.git
  GIT_TAG 625269b7725a6e2a3f2724e8d45b602c1b20ead5)
FetchContent_MakeAvailable(libusbhsfs)

# the patch, applied once (a reconfigure finds it applied); a source it does not fit stops the configure
find_package(Git REQUIRED)
set(cane_usbhsfs_patch "${CMAKE_CURRENT_LIST_DIR}/../platform/switch/libusbhsfs.patch")
set(git_apply "${GIT_EXECUTABLE}" -c "safe.directory=${libusbhsfs_SOURCE_DIR}" apply --ignore-space-change)
execute_process(COMMAND ${git_apply} --check "${cane_usbhsfs_patch}"
  WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}" RESULT_VARIABLE fits OUTPUT_QUIET ERROR_QUIET)
if(fits EQUAL 0)
  execute_process(COMMAND ${git_apply} "${cane_usbhsfs_patch}" WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}" RESULT_VARIABLE applied)
  if(NOT applied EQUAL 0)
    message(FATAL_ERROR "libusbhsfs: libusbhsfs.patch did not apply")
  endif()
else()
  execute_process(COMMAND ${git_apply} --reverse --check "${cane_usbhsfs_patch}"
    WORKING_DIRECTORY "${libusbhsfs_SOURCE_DIR}" RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
  if(NOT applied EQUAL 0)
    message(FATAL_ERROR "libusbhsfs: the source does not match libusbhsfs.patch")
  endif()
endif()
target_sources(libusbhsfs PRIVATE "${libusbhsfs_SOURCE_DIR}/source/usbhsfs_uasp.c")
target_include_directories(libusbhsfs PRIVATE "${libusbhsfs_SOURCE_DIR}/source")
target_compile_definitions(libusbhsfs PRIVATE get_fattime=usbhsfs_get_fattime)
file(STRINGS "${DEVKITPRO}/libnx/include/switch/services/usb.h" libnx_usb_request_enums REGEX "^enum usb_request_type")
if(libnx_usb_request_enums)
  target_compile_definitions(libusbhsfs PRIVATE USBHSFS_LIBNX_USB_REQUEST_ENUMS)
endif()
