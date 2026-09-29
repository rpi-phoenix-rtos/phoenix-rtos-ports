# Poppler's CMake options for tools/gpu-lane/atril-wayland: sourced by build.sh (the Pi
# build, + -DENABLE_UTILS=OFF) and by hosttest/run.sh (the native build, + -DENABLE_UTILS=ON
# for pdftocairo/pdfinfo), so both render with the same configuration.
#   on:  poppler-glib (cairo output), libjpeg (DCT), openjpeg (JPX), lcms2 (ICC), fontconfig
#   off: Qt5/Qt6, the C++ wrapper, NSS/GPGME signatures, curl, libtiff output, boost (Splash
#        speed-ups), harfbuzz (font subsetting when saving forms: needs harfbuzz-subset, which
#        the GTK stack's harfbuzz does not build), introspection, gtk-doc, every test program
# SPDX-License-Identifier: BSD-3-Clause
POPPLER_OPTS=(
	-DENABLE_GLIB=ON -DENABLE_CPP=OFF -DENABLE_QT5=OFF -DENABLE_QT6=OFF
	-DENABLE_LIBJPEG=ON -DENABLE_LIBOPENJPEG=ON -DENABLE_LCMS=ON
	-DENABLE_LIBTIFF=OFF -DENABLE_LIBCURL=OFF -DENABLE_NSS3=OFF -DENABLE_GPGME=OFF -DENABLE_BOOST=OFF
	-DENABLE_HARFBUZZ=OFF -DENABLE_GOBJECT_INTROSPECTION=OFF -DENABLE_GTK_DOC=OFF
	-DENABLE_UNSTABLE_API_ABI_HEADERS=OFF -DBUILD_GTK_TESTS=OFF -DBUILD_QT5_TESTS=OFF -DBUILD_QT6_TESTS=OFF
	-DBUILD_CPP_TESTS=OFF -DBUILD_MANUAL_TESTS=OFF -DRUN_GPERF_IF_PRESENT=OFF -DBUILD_SHARED_LIBS=OFF
	-DFONT_CONFIGURATION=fontconfig
)
