# ffmpeg component set of the M10 player builds (sourced by build-ffplay.sh and
# hosttest/run.sh, so the Phoenix binary and its host control are configured alike).
# Decoders for the usual demo containers; the filters ffplay's graphs use (scale and
# aresample are auto-inserted on a format mismatch: without them a graph fails to
# configure); every component is LGPL (no --enable-gpl / --enable-nonfree).
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%
FF_DECODERS=h264,hevc,vp8,vp9,mpeg4,mjpeg,rawvideo,aac,aac_latm,mp3,mp3float,opus,vorbis,flac,pcm_s16le,pcm_s24le,pcm_f32le
FF_DEMUXERS=mov,matroska,mpegts,avi,h264,hevc,m4v,mjpeg,wav,ogg,mp3,aac,flac
FF_PARSERS=h264,hevc,vp8,vp9,mpeg4video,mjpeg,aac,aac_latm,mpegaudio,opus,vorbis,flac
FF_BSFS=h264_mp4toannexb,hevc_mp4toannexb,vp9_superframe_split,aac_adtstoasc
FF_FILTERS=buffer,buffersink,abuffer,abuffersink,format,aformat,null,anull,scale,aresample,crop,transpose,hflip,vflip,rotate,setpts,asetpts
FF_PROTOCOLS=file,pipe
# the configure switches both builds share (the port's line + the player's libraries)
FF_COMMON=(--disable-autodetect --disable-everything --enable-pthreads
	--enable-avfilter --enable-swscale --enable-swresample --disable-avdevice --disable-postproc
	--enable-decoder="${FF_DECODERS}" --enable-demuxer="${FF_DEMUXERS}" --enable-parser="${FF_PARSERS}"
	--enable-bsf="${FF_BSFS}" --enable-filter="${FF_FILTERS}" --enable-protocol="${FF_PROTOCOLS}"
	--disable-network --disable-doc)
