# Host build of prep/ + its test driver: make -C tests/prep  ->  build/host_prep
ROOT := ../..
AAC  := $(ROOT)/third_party/vo-aacenc
SRC  := host_prep.c $(ROOT)/prep/source/prep.c $(wildcard $(ROOT)/codec/source/*.c) \
        $(wildcard $(AAC)/aacenc/src/*.c) $(wildcard $(AAC)/aacenc/basic_op/*.c) $(AAC)/common/cmnMemory.c
INC  := -I$(ROOT)/prep/include -I$(ROOT)/codec/include -I$(ROOT)/tools/r3dconv -I$(ROOT)/third_party/minih264 \
        -I$(AAC)/aacenc/inc -I$(AAC)/aacenc/basic_op -I$(AAC)/common/include

$(ROOT)/build/host_prep: $(SRC) $(ROOT)/prep/include/prep.h
	$(CC) -O2 -DPREP_PLAIN_C -w $(INC) $(SRC) -lm -lpthread -o $@
