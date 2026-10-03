# SPDX-License-Identifier: GPL-2.0+

obj-y += gx6706.o
obj-y += gx6706_usb.o
obj-y += gx6706_fuse.o

ccflags-$(CONFIG_VIDEO_GX6702) += -I$(srctree)/board/nationalchip/gx6702
obj-$(CONFIG_VIDEO_GX6702) += ../gx6702/gx6702_video.o
obj-$(CONFIG_VIDEO_GX6702) += ../gx6702/gx6702_hdmi.o
obj-$(CONFIG_VIDEO_GX6702) += ../gx6702/gx6702_ecos_goldens.o
obj-$(CONFIG_VIDEO_GX6702) += ../gx6702/cmd_gxvideo.o
obj-$(CONFIG_VIDEO_GX6702) += ../gx6702/gx6702_cec.o
obj-$(CONFIG_VIDEO_GX6702) += ../gx6702/cmd_gxcec.o
