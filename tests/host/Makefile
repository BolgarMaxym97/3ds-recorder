CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -DBENCH_HOST \
           -I../../libbench/include -I../benchplugin/Sources -I../../codec/include
TESTS   := test_bench test_convert test_codec test_audio test_huff test_fused test_unwrap test_autostart test_front test_plgring test_appletguard test_join

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
