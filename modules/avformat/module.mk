#
# module.mk
#
# Copyright (C) 2010 Creytiv.com
#

MOD		:= avformat
$(MOD)_SRCS	+= avformat.c audio.c queue.c
$(MOD)_LFLAGS	+= -lavdevice -lavformat -lavcodec -lavutil -lavresample
CFLAGS          += -DUSE_AVFORMAT

include mk/mod.mk
