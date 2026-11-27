#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

# libopus
OPUSLIB := $(CODECDIR)/libopus.a
OPUSLIB_SRC := $(call preprocess, $(RBCODECLIB_DIR)/codecs/libopus/SOURCES)
OPUSLIB_OBJ := $(call c2obj, $(OPUSLIB_SRC))

# codec specific compilation flags
# prepend paths to avoid name clash issues
$(OPUSLIB) : CODECFLAGS := -DHAVE_CONFIG_H \
   -I$(RBCODECLIB_DIR)/codecs/libopus \
   -I$(RBCODECLIB_DIR)/codecs/libopus/celt \
   -I$(RBCODECLIB_DIR)/codecs/libopus/silk \
   $(CODECFLAGS)

# bcm overlay v142: on the iPod Video (PP5022: 80 KB of codec IRAM, about 40 KB of it unused by this codec before) the
# decoder's hot functions run from IRAM: the inverse MDCT / FFT, the band decoding (PVQ, rotations, range decoder),
# the comb filter and de-emphasis, and SILK's synthesis and 48 kHz up-sampling.  Every function gets its own section
# (-ffunction-sections); the listed ones are renamed to .icode in the objects that go into the archive, the rest stay
# in .text.  The lists name functions, so the file contents are not touched.
ifeq ($(MODELNAME),ipodvideo)
$(OPUSLIB) : CODECFLAGS += -ffunction-sections
OPUS_IRAM_FN := opus_fft_impl kf_bfly2 kf_bfly3 kf_bfly4 kf_bfly5 clt_mdct_backward_c \
    comb_filter comb_filter_const_c deemphasis celt_synthesis tf_decode \
    quant_all_bands quant_band quant_band_n1 quant_partition quant_band_stereo compute_theta denormalise_bands \
    anti_collapse haar1 deinterleave_hadamard interleave_hadamard compute_qn intensity_stereo stereo_merge \
    celt_lcg_rand bitexact_cos bitexact_log2tan special_hybrid_folding \
    alg_unquant exp_rotation exp_rotation1 normalise_residual extract_collapse_mask renormalise_vector \
    decode_pulses cwrsi \
    ec_read_byte ec_read_byte_from_end ec_dec_normalize ec_decode ec_decode_bin ec_dec_update ec_dec_bit_logp \
    ec_dec_icdf ec_dec_uint ec_dec_bits ec_laplace_decode \
    isqrt32 celt_sqrt celt_rsqrt_norm celt_cos_norm celt_rcp frac_div32 celt_exp2 \
    unquant_coarse_energy unquant_fine_energy unquant_energy_finalise \
    silk_decode_core silk_resampler silk_resampler_private_IIR_FIR silk_resampler_private_IIR_FIR_INTERPOL \
    silk_resampler_private_up2_HQ silk_resampler_private_up2_HQ_wrapper
OPUS_IRAM_RENAME := $(foreach f,$(OPUS_IRAM_FN),--rename-section .text.$(f)=.icode)
$(OPUSLIB): $(OPUSLIB_OBJ)
	$(SILENT)$(shell rm -f $@)
	$(SILENT)for o in $^; do $(OC) $(OPUS_IRAM_RENAME) $$o $$o.ir.o || exit 1; done
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $(addsuffix .ir.o,$^) >/dev/null
else
$(OPUSLIB): $(OPUSLIB_OBJ)
	$(SILENT)$(shell rm -f $@)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $^ >/dev/null
endif
