#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

# libfaad
FAADLIB := $(CODECDIR)/libfaad.a
FAADLIB_SRC := $(call preprocess, $(RBCODECLIB_DIR)/codecs/libfaad/SOURCES)
FAADLIB_OBJ := $(call c2obj, $(FAADLIB_SRC))
OTHER_SRC += $(FAADLIB_SRC)
OTHER_INC += -I$(RBCODECLIB_DIR)/codecs/libfaad

# bcm overlay v144: BCM_FAAD_IRAM=1 builds a variant with more of the AAC decoder in IRAM (upstream already puts some
# of it there on the PP5022), for the System > Debug > Codec A/B comparison.
BCM_FAAD_IRAM ?= 0
ifeq ($(MODELNAME)$(BCM_FAAD_IRAM),ipodvideo1)
$(FAADLIB) : CODECFLAGS += -ffunction-sections
FAAD_IRAM_FN := ifilter_bank quant_to_spec huffman_sign_bits_quad huffman_sign_bits_pair tns_decode_frame tns_ar_filter \
    individual_channel_stream reconstruct_single_channel reconstruct_channel_pair ms_decode is_decode pns_decode \
    huffman_2step_quad huffman_2step_pair huffman_binary_quad huffman_binary_pair iquant
FAAD_IRAM_RENAME := $(foreach f,$(FAAD_IRAM_FN),--rename-section .text.$(f)=.icode)
$(FAADLIB): $(FAADLIB_OBJ)
	$(SILENT)$(shell rm -f $@)
	$(SILENT)for o in $^; do $(OC) $(FAAD_IRAM_RENAME) $$o $$o.ir.o || exit 1; done
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $(addsuffix .ir.o,$^) >/dev/null
else
$(FAADLIB): $(FAADLIB_OBJ)
	$(SILENT)$(shell rm -f $@)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $^ >/dev/null
endif
