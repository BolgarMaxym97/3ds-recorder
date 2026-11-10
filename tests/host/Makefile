CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -O2 -DBENCH_HOST \
           -I../../libbench/include -I../benchplugin/Sources -I../../codec/include
TESTS   := test_bench test_convert test_codec test_audio test_huff test_fused

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

clean:
	rm -f $(TESTS)
