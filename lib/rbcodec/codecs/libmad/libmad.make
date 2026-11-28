#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

# we need to build two different mad libraries
# (one for codec, one for mpegplayer)
# so a little trickery is necessary

MADFLAGS = $(CODECFLAGS) -I$(RBCODECLIB_DIR)/codecs/libmad
MADFLAGS += -UDEBUG -DNDEBUG -DHAVE_LIMITS_H -DHAVE_ASSERT_H

# MPEGplayer
MPEGMADFLAGS = $(MADFLAGS) -DMPEGPLAYER

# libmad
MADLIB := $(CODECDIR)/libmad.a
MADLIB_SRC := $(call preprocess, $(RBCODECLIB_DIR)/codecs/libmad/SOURCES)
MADLIB_OBJ := $(call c2obj, $(MADLIB_SRC))
OTHER_SRC += $(MADLIB_SRC)

# bcm overlay v144: BCM_MAD_IRAM=1 builds a variant with libmad's hot code in the codec IRAM, for the System > Debug >
# Codec A/B comparison against the stock layout.  Upstream keeps libmad's code out of IRAM on PP502x ("performs slower
# in IRAM"), so the stock build stays the default until a measurement says otherwise.
BCM_MAD_IRAM ?= 0
ifeq ($(MODELNAME)$(BCM_MAD_IRAM),ipodvideo1)
$(MADLIB) : CODECFLAGS += -ffunction-sections
MAD_IRAM_FN := III_decode mad_synth_frame III_imdct_s III_imdct_l III_aliasreduce III_reorder III_overlap III_overlap_z \
    III_freqinver III_exponents III_requantize III_huffdecode III_stereo mad_layer_III synth_full synth_half dct32 \
    fastsdct sdctII mad_bit_read mad_bit_skip mad_bit_init
MAD_IRAM_RENAME := $(foreach f,$(MAD_IRAM_FN),--rename-section .text.$(f)=.icode)
$(MADLIB): $(MADLIB_OBJ)
	$(SILENT)$(shell rm -f $@)
	$(SILENT)for o in $^; do case $$o in *_arm.o) $(OC) --rename-section .text=.icode $$o $$o.ir.o ;; \
	    *) $(OC) $(MAD_IRAM_RENAME) $$o $$o.ir.o ;; esac || exit 1; done
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $(addsuffix .ir.o,$^) >/dev/null
else
$(MADLIB): $(MADLIB_OBJ)
	$(SILENT)$(shell rm -f $@)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $^ >/dev/null
endif

# libmad-mpeg
MPEGMADLIB := $(CODECDIR)/libmad-mpeg.a
MPEGMADLIB_SRC := $(call preprocess, $(RBCODECLIB_DIR)/codecs/libmad/SOURCES)
MPEGMADLIB_OBJ := $(addsuffix .o,$(basename $(subst $(RBCODECLIB_DIR)/codecs/libmad,$(RBCODEC_BLD)/codecs/libmad-mpeg,$(MPEGMADLIB_SRC))))

$(MPEGMADLIB): $(MPEGMADLIB_OBJ)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $^ >/dev/null

# pattern rules

$(CODECDIR)/libmad-mpeg/%.o : $(RBCODECLIB_DIR)/codecs/libmad/%.c
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<)) \
		$(CC) $(MPEGMADFLAGS) -c $< -o $@

$(CODECDIR)/libmad-mpeg/%.o : $(RBCODECLIB_DIR)/codecs/libmad/%.S
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<)) \
		$(CC) $(MPEGMADFLAGS) -c $< -o $@

$(CODECDIR)/libmad/%.o: $(RBCODECLIB_DIR)/codecs/libmad/%.c
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<)) \
		$(CC) $(MADFLAGS) -c $< -o $@

$(CODECDIR)/libmad/%.o: $(RBCODECLIB_DIR)/codecs/libmad/%.S
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<)) \
		$(CC) $(MADFLAGS) -c $< -o $@
