#!/usr/bin/env python

# Copyright Contributors to the OpenImageIO project.
# SPDX-License-Identifier: Apache-2.0
# https://github.com/OpenImageIO/oiio


from __future__ import absolute_import
import os

files = [ 
          "RAW_CANON_40D_SRAW_V103.CR2",
          "RAW_CANON_EOS_7D.CR2",
          "RAW_FUJI_E550.RAF",
          "RAW_FUJI_E900.RAF",
          "RAW_FUJI_F700.RAF",
          "RAW_FUJI_S9600.RAF",
          "RAW_KODAK_C330_FORMAT420_YRGB.RAW",
          "RAW_KODAK_C330_FORMAT422_YRGB.RAW",
          "RAW_KODAK_C330_FORMAT_NONE_YRGB.RAW",
          "RAW_NIKON_D1X.NEF",
          "RAW_NIKON_D3X.NEF",
          "RAW_OLYMPUS_E3.ORF",
          "RAW_PANASONIC_G1.RW2",
          "RAW_PENTAX_K200D.PEF",
          "RAW_PENTAX_K5IIS.PEF",
          "RAW_SONY_A300.ARW",
]
outputs = []

# Things vary a lot with libraw versions.
# FIXME -- return to this later
if (os.getenv('GITHUB_ACTIONS') == 'true'):
    failthresh = 0.024
    files.remove ("RAW_PANASONIC_G1.RW2")

# Fairly high hard fail, since libraw seems to diddle with its debayering
# from version to version, it's hard to make a single reference image.
hardfail = 0.017

# For each test image, read it and print all metadata, resize it (to make
# the ref images small) and compared to the reference.
for f in files:
    outputname = f+".tif"
    command += oiiotool ("-iconfig raw:ColorSpace sRGB-linear "
                         + "-i:info=2 " + OIIO_TESTSUITE_IMAGEDIR + "/" + f
                         + " -resample '5%' -d uint8 "
                         + "-o " + outputname)
    outputs += [ outputname ]

# For each test image, read it and write a DNG file. Then read the DNG,
# resize it (to make the ref images small) convert to TIF and compare it to the reference
for f in files:
    dng_output = f + "_temp.dng"
    tif_output = f + ".tif"
    command += oiiotool ("-iconfig raw:Demosaic none "
                         + "-i:info=2 " + OIIO_TESTSUITE_IMAGEDIR + "/" + f
                         + " -o " + dng_output)
    command += oiiotool ("-iconfig raw:ColorSpace sRGB "
                         + "-i:info=2 " + dng_output
                         + " -resample '5%' -d uint8"
                         + " -o " + tif_output)
    outputs += [ dng_output, tif_output ]

outputs += [ "out.txt" ]
