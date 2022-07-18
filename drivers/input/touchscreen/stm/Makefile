# FTM5 support
ccflags-y := -Wno-error=vla -Wno-error=unused-variable -Wno-error=unused-variable -Wno-error=implicit-fallthrough= -Wno-error=address
obj-$(CONFIG_TOUCHSCREEN_FTS)	+= ftm5.o
ftm5-objs = fts.o fts_proc.o
ftm5-objs += \
	fts_lib/ftsCompensation.o fts_lib/ftsCore.o fts_lib/ftsError.o \
	fts_lib/ftsFrame.o fts_lib/ftsIO.o fts_lib/ftsTest.o fts_lib/ftsTime.o \
	fts_lib/ftsTool.o fts_lib/ftsFlash.o fts_lib/ftsGesture.o

# Legacy driver
obj-$(CONFIG_TOUCHSCREEN_FTM4)	+= ftm4.o
ftm4-objs			+= ftm4_ts.o ftm4_fwu.o ftm4_pdc.o
