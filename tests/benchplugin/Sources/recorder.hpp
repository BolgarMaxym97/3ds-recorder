#ifndef RECORDER_HPP
#define RECORDER_HPP

/*
 * Phase-0 recorder bench (T2 + T4 CPU capture + T7 minimal pipeline).
 *
 * Capture: CTRPF OSD callback runs on every game frame (in the game's own
 * thread), converts the framebuffer to RGB565 straight into a byte ring buffer.
 * Writer: separate thread drains the ring to SD in 256 KB blocks with the T1
 * configuration (64 MB first allocation, +16 MB chunks, FS_WRITE_FLUSH).
 */

#include <3ds.h>

namespace Recorder
{
    enum Mode
    {
        ModeCountOnly = 0, ///< no capture: measures game frame pacing with the plugin idle
        ModeHalfTop,
        ModeHalfBoth,
        ModeFullTop,
        ModeFullBoth,
        ModeHybrid,        ///< top full-res + bottom half-res
        ModeWriteOnly,     ///< no capture: writer streams dummy data at writeRateKBs (T8: SD cost alone)
        ModeCount
    };

    struct Options
    {
        Mode mode = ModeHalfBoth;
        bool writeToSd = true;  ///< false: capture into the ring, writer discards (isolates capture cost)
        int  writerCore = 0;    ///< 0 = app core, 1 = sys core
        u32  blockKB = 256;     ///< SD write block size (64 or 256)
        u32  writeRateKBs = 2662; ///< write_only target rate, KB/s (2.6 MB/s = half-res both screens)
        int  filePriority = -1; ///< FSFILE_SetPriority on the recording file; -1 = leave default
        bool useDma = true;     ///< true: svcStartInterProcessDma copies the frame, a separate thread converts
        int  convertCore = 2;   ///< DMA mode: 0 / 1 = one converter on that core, 2 = two converters (cores 1 and 0)
        int  encVariant = 0;    ///< XR32 memory-hint variant (0 none, 1 PLD, 2 touch), set from the microbench
        bool keepRecording = false; ///< keep this run's file as rec_<tid>_<mode>.r3dtest (codec research)
        int  codec = 2;         ///< DMA mode: 0 = raw RGB565, 1 = XOR-RLE, 2 = XR32 (pixel pairs, faster); keyframe every 30
    };

    const char *ModeName(Mode mode);

    /* Compact result of the last finished run (for the autotest table). */
    struct RunResult
    {
        Mode   mode = ModeCountOnly;
        double fpsActive = 0;   ///< game fps excluding pauses > 500 ms (menu, loading)
        double fpsSteady = 0;   ///< same, without the first 6 s of the run
        double botSkipPct = 0;  ///< bottom frames skipped on purpose (top backlog), %
        double topThrottlePct = 0; ///< top frames skipped on purpose (SD behind: ring > 70%), %
        double dropPct = 0;     ///< frames not recorded / frames offered, %
        double ratio = 0;       ///< codec ratio
        double mbps = 0;        ///< stream to SD
        double ringMaxPct = 0;
        u32    convFrames[2] = { 0, 0 };
        long long cpuLimit = -1;
        bool   valid = false;
    };
    const RunResult &LastRun(void);

    bool IsRunning(void);
    /* Creates and allocates the next recording file in the background (call at plugin start). */
    void PrepareNext(void);
    /* Plugin exit: removes an unused prepared file. */
    void Shutdown(void);
    /* Returns false and sets `error` if the session could not start. */
    bool Start(const Options &opt, char *error, u32 errorSize);
    /* Stops, writes CSVs, fills `summary` with a short human-readable result. */
    void Stop(char *summary, u32 summarySize);
}

#endif
