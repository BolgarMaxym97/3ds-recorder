CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -DBENCH_HOST \
           -I../../libbench/include -I../benchplugin/Sources -I../../codec/include
TESTS   := test_bench test_convert test_codec test_audio test_huff test_fused test_unwrap test_autostart test_front test_plgring test_appletguard test_join test_stats test_textbreak test_i18n test_inputs test_srvtoken test_update test_support

.PHONY: test clean

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_bench: test_bench.c ../../libbench/source/bench.c ../../libbench/include/bench.h
	$(CC) $(CFLAGS) -o $@ test_bench.c ../../libbench/source/bench.c -lm

test_convert: test_convert.c ../../codec/source/convert.c ../../codec/include/convert.h
	$(CC) $(CFLAGS) -o $@ test_convert.c ../../codec/source/convert.c

test_codec: test_codec.c ../../codec/source/codec.c ../../codec/include/codec.h
	$(CC) $(CFLAGS) -o $@ test_codec.c ../../codec/source/codec.c

test_huff: test_huff.c ../../codec/source/huff.c ../../codec/include/huff.h ../../codec/source/codec.c
	$(CC) $(CFLAGS) -o $@ test_huff.c ../../codec/source/huff.c ../../codec/source/codec.c

test_fused: test_fused.c ../../codec/source/codec.c ../../codec/source/convert.c
	$(CC) $(CFLAGS) -o $@ test_fused.c ../../codec/source/codec.c ../../codec/source/convert.c

test_audio: test_audio.c ../../tools/r3dconv/audio_place.h ../../tools/r3dconv/audio_order.h ../../codec/include/r3d_format.h
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -o $@ test_audio.c -lm

test_unwrap: test_unwrap.c ../../prep/source/unwrap.c ../../prep/include/unwrap.h ../../codec/include/r3d_format.h
	$(CC) $(CFLAGS) -I../../prep/include -o $@ test_unwrap.c ../../prep/source/unwrap.c

clean:
	rm -f $(TESTS)

test_autostart: test_autostart.c ../../sysmod/source/autostart.h
	$(CC) $(CFLAGS) -I../../sysmod/source -o $@ test_autostart.c

test_front: test_front.c ../../sysmod/source/front.h
	$(CC) $(CFLAGS) -I../../sysmod/source -o $@ test_front.c

test_plgring: test_plgring.c ../../sysmod/source/plgring.h
	$(CC) $(CFLAGS) -I../../sysmod/source -o $@ test_plgring.c

test_appletguard: test_appletguard.c ../../sysmod/source/appletguard.h
	$(CC) $(CFLAGS) -I../../sysmod/source -o $@ test_appletguard.c

test_join: test_join.c ../../prep/source/join.c ../../prep/include/join.h ../../codec/include/r3d_format.h
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -I../../prep/include -o $@ test_join.c ../../prep/source/join.c

test_stats: test_stats.c ../../app/source/stats_core.c ../../app/source/stats_core.h
	$(CC) $(CFLAGS) -I../../app/source -o $@ test_stats.c ../../app/source/stats_core.c

test_textbreak: test_textbreak.c ../../app/source/textbreak.c ../../app/source/textbreak.h
	$(CC) $(CFLAGS) -I../../app/source -o $@ test_textbreak.c ../../app/source/textbreak.c

test_i18n: test_i18n.c ../../app/source/i18n.c ../../app/source/i18n.h ../../app/source/lang/*.c
	$(CC) $(CFLAGS) -I../../app/source -o $@ test_i18n.c ../../app/source/i18n.c ../../app/source/lang/*.c

test_inputs: test_inputs.c ../../codec/source/inputs.c ../../codec/include/inputs.h ../../codec/include/r3d_format.h
	$(CC) $(CFLAGS) -o $@ test_inputs.c ../../codec/source/inputs.c -lm

test_srvtoken: test_srvtoken.c ../../app/source/srvtoken.h
	$(CC) $(CFLAGS) -I../../app/source -o $@ test_srvtoken.c

MONO := ../../third_party/monocypher
test_update: test_update.c ../../app/source/update_core.c ../../app/source/update_core.h ../../app/source/sha256.c
	$(CC) $(CFLAGS) -I../../app/source -I$(MONO) -o $@ test_update.c ../../app/source/update_core.c \
		../../app/source/sha256.c $(MONO)/monocypher.c $(MONO)/monocypher-ed25519.c

test_support: test_support.c ../../app/source/support_core.c ../../app/source/support_core.h
	$(CC) $(CFLAGS) -I../../app/source -o $@ test_support.c ../../app/source/support_core.c
