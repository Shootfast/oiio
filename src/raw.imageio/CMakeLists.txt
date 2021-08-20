# Copyright Contributors to the OpenImageIO project.
# SPDX-License-Identifier: Apache-2.0
# https://github.com/OpenImageIO/oiio

if (LIBRAW_FOUND)
    add_oiio_plugin (rawinput.cpp rawoutput.cpp
		INCLUDE_DIRS ${LibRaw_INCLUDE_DIR} ${TIFF_INCLUDE_DIR}
		LINK_LIBRARIES ${LibRaw_r_LIBRARIES} ${TIFF_LIBRARIES} ZLIB::ZLIB
                     DEFINITIONS "-DUSE_LIBRAW=1" ${LibRaw_r_DEFINITIONS})
else ()
    add_oiio_plugin (rawoutput.cpp
                     INCLUDE_DIRS ${TIFF_INCLUDE_DIR}
					 LINK_LIBRARIES ${TIFF_LIBRARIES} ZLIB::ZLIB)
endif ()
