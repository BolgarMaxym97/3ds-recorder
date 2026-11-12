#ifndef R3D_FORMAT_H
#define R3D_FORMAT_H
/*
 * .r3d recording format (shared by the recorder sysmodule, the bench plugin
 * and the host tools). Little-endian, every record 16-byte aligned.
 *
 *   FileHeader (32 bytes)
 *   records: RecHeader (16 bytes) + payload (padded to 16 for delta frames)
 *     R3DF  raw RGB565 frame
 *     R3DX  XOR-RLE frame            (screen byte bit 6 = keyframe)
 *     R3DY  XR32 frame (pixel pairs) (screen byte bit 6 = keyframe)
 *     R3DQ  Q565 frame: the picture on its own (version 5, codec.h; always a keyframe, bit 6 set)
 *     R3DC  C420 frame: the picture on its own at half the bytes, lossy like 4:2:0 video (version 6,
 *           codec.h; always a keyframe, bit 6 set; len = cols * 240)
 *     R3DH  XR32 frame with a Huffman stage over its bytes (version 5, codec/include/huff.h;
 *           screen byte bit 6 = keyframe)
 *     R3DP  padding: skip `len` bytes (ring buffer wrap)
 *     R3DA  audio frame (version 4): `cols` stereo PCM16 samples (L R L R ...) at
 *           the header's audioRate, timeUs = when the recorder saw it; silence is
 *           not stored (a gap in time = silence). screen = R3D_STREAM_AUDIO,
 *           srcFormat = DSP region the frame was read from (diagnostics). When
 *           len > cols * 4, an R3dAudioTrailer follows the samples.
 *     R3DM  DSP shared memory snapshot (version 4, diagnostics): `len` bytes,
 *           both DSP regions (2 x 0x8000), screen = R3D_STREAM_DSPMEM.
 *     R3DE  end of a properly closed recording, the file's last record: an
 *           R3dEndInfo (len 16), timeUs = the recording's length. Readers take it
 *           from the file's last 32 bytes; a file without it was cut off.
 * Frames are column-major, RGB565, as the LCD stores them (column = screen x,
 * word 0 = bottom row). A file that was not closed properly ends with
 * whatever the SD card held (FAT does not zero preallocated clusters): readers
 * stop at the first record with an unknown magic.
 */
#include <stdint.h>

#define R3D_MAGIC_FILE   0x42443352u   /* "R3DB" */
#define R3D_MAGIC_FRAME  0x46443352u   /* "R3DF" */
#define R3D_MAGIC_XFRAME 0x58443352u   /* "R3DX" */
#define R3D_MAGIC_YFRAME 0x59443352u   /* "R3DY" */
#define R3D_MAGIC_HFRAME 0x48443352u   /* "R3DH" */
#define R3D_MAGIC_QFRAME 0x51443352u   /* "R3DQ" */
#define R3D_MAGIC_CFRAME 0x43443352u   /* "R3DC" */
#define R3D_MAGIC_PAD    0x50443352u   /* "R3DP" */
#define R3D_MAGIC_AUDIO  0x41443352u   /* "R3DA" */
#define R3D_MAGIC_DSPMEM 0x4D443352u   /* "R3DM" */
#define R3D_MAGIC_END    0x45443352u   /* "R3DE" */

#define R3D_VERSION      6u   /* 5: R3DQ and R3DH frames, bottom-screen-only mode; 6: R3DC frames */
#define R3D_STREAM_AUDIO  2u
#define R3D_STREAM_DSPMEM 3u
/* DSP output: 268111856 / 8192 Hz, 160 samples per DSP frame */
#define R3D_AUDIO_RATE_MHZ 32728498u
#define R3D_SCREEN_KEY   0x40u

/* After the samples of an R3DA record (optional, 16 bytes). */
typedef struct {
    uint16_t counter[2];   /* both DSP regions' frame counters when the frame was taken */
    uint16_t pollGapUs;    /* longest gap between the recorder's audio polls before it */
    uint16_t flags;        /* R3D_AUDIO_* */
    uint8_t  reserved[8];
} R3dAudioTrailer;
#define R3D_AUDIO_BOTH_CHANGED 1u   /* both regions changed in one poll (order guessed) */
#define R3D_AUDIO_RAW          2u   /* diagnostics (audio_debug): a raw read, not a frame */

/* Payload of the R3DE record. */
typedef struct {
    uint32_t framesTop;
    uint32_t framesBottom;
    uint32_t dropped;      /* frames the recorder could not keep */
    uint32_t reserved;
} R3dEndInfo;

/* mode: what the file holds */
enum { R3D_MODE_FULL_TOP = 3, R3D_MODE_FULL_BOTH = 4, R3D_MODE_FULL_BOTTOM = 5 };

typedef struct {
    uint32_t magic;        /* R3D_MAGIC_FILE */
    uint32_t version;      /* 1-2 bench, 3 recorder */
    uint64_t titleId;      /* 0: whole console */
    uint8_t  isNew3ds;
    uint8_t  mode;
    uint8_t  fps;          /* target capture rate */
    uint8_t  reserved0;
    uint32_t audioRate;    /* version 4: audio sample rate in mHz, 0 = no audio */
    uint64_t startTimeMs;  /* osGetTime(): ms since 1900-01-01 */
} R3dFileHeader;

typedef struct {
    uint32_t magic;
    uint32_t len;          /* payload bytes (frame) or bytes to skip (pad) */
    uint32_t timeUs;       /* since recording start */
    uint8_t  screen;       /* 0 top, 1 bottom; | R3D_SCREEN_KEY */
    uint8_t  srcFormat;    /* LCD format register (low byte) */
    uint16_t cols;         /* 400 / 320; rows = 240 */
} R3dRecHeader;

#ifdef __cplusplus
static_assert(sizeof(R3dFileHeader) == 32, "R3dFileHeader");
static_assert(sizeof(R3dRecHeader) == 16, "R3dRecHeader");
#else
_Static_assert(sizeof(R3dFileHeader) == 32, "R3dFileHeader");
_Static_assert(sizeof(R3dRecHeader) == 16, "R3dRecHeader");
_Static_assert(sizeof(R3dAudioTrailer) == 16, "R3dAudioTrailer");
_Static_assert(sizeof(R3dEndInfo) == 16, "R3dEndInfo");
#endif

#endif
