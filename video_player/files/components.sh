# ffmpeg component set of the M10 player builds (sourced by build-ffplay.sh and
# hosttest/run.sh, so the Phoenix binary and its host control are configured alike).
# Decoders for the usual demo containers; the filters ffplay's graphs use (scale and
# aresample are auto-inserted on a format mismatch: without them a graph fails to
# configure); every component is LGPL (no --enable-gpl / --enable-nonfree).
# Also MPEG-1/2 video in .mpg/.vob (mpegps) and broadcast .ts (mpegts), AC-3/E-AC-3/DTS
# and ALAC audio, FLV and IVF, and two deinterlacers (yadif, bwdif: -vf yadif) for
# interlaced MPEG-2/H.264. zlib: Matroska's compressed tracks and MOV's compressed
# 'cmov' header need it (configure turns it off under --disable-autodetect).
#
# Copyright 2026 Phoenix Systems
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%
FF_DECODERS=h264,hevc,vp8,vp9,mpeg4,mpeg2video,mpeg1video,mjpeg,rawvideo,aac,aac_latm,mp3,mp3float,opus,vorbis,flac,ac3,eac3,dca,alac,pcm_s16le,pcm_s24le,pcm_f32le
FF_DEMUXERS=mov,matroska,mpegts,mpegps,avi,flv,ivf,h264,hevc,m4v,mjpeg,wav,ogg,mp3,aac,flac
FF_PARSERS=h264,hevc,vp8,vp9,mpeg4video,mpegvideo,mjpeg,aac,aac_latm,mpegaudio,opus,vorbis,flac,ac3,dca
FF_BSFS=h264_mp4toannexb,hevc_mp4toannexb,vp9_superframe_split,aac_adtstoasc
FF_FILTERS=buffer,buffersink,abuffer,abuffersink,format,aformat,null,anull,scale,aresample,crop,transpose,hflip,vflip,rotate,setpts,asetpts,yadif,bwdif
FF_PROTOCOLS=file,pipe
# the configure switches both builds share (the port's line + the player's libraries)
FF_COMMON=(--disable-autodetect --disable-everything --enable-pthreads --enable-zlib
	--enable-avfilter --enable-swscale --enable-swresample --disable-avdevice --disable-postproc
	--enable-decoder="${FF_DECODERS}" --enable-demuxer="${FF_DEMUXERS}" --enable-parser="${FF_PARSERS}"
	--enable-bsf="${FF_BSFS}" --enable-filter="${FF_FILTERS}" --enable-protocol="${FF_PROTOCOLS}"
	--disable-network --disable-doc)
