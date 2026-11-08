/*
 * bench.3gx — phase-0 in-game benchmark plugin (T2, T3, T4 CPU capture).
 *
 * Open the menu with SELECT. Recording also toggles with L + R + Down.
 * All results go to sdmc:/3ds/recorder/bench/.
 */
#include <CTRPluginFramework.hpp>
#include <stdarg.h>
#include <stdio.h>

#include "dmaprobe.hpp"
#include "homeprobe.hpp"
#include "microbench.hpp"
#include "memprobe.hpp"
#include "recorder.hpp"
#include "sdio.h"

namespace CTRPluginFramework
{
    namespace
    {
        Recorder::Options g_opt;
        MenuEntry *g_modeEntry = nullptr;
        MenuEntry *g_sdEntry = nullptr;
        MenuEntry *g_coreEntry = nullptr;
        MenuEntry *g_rateEntry = nullptr;
        MenuEntry *g_blockEntry = nullptr;
        MenuEntry *g_prioEntry = nullptr;
        MenuEntry *g_captureEntry = nullptr;
        MenuEntry *g_convCoreEntry = nullptr;
        MenuEntry *g_codecEntry = nullptr;

        const u32 RATES_KBS[] = { 1229, 2662, 5939 }; // 1.2 (HQ JPEG est.), 2.6 (half_both), 5.8 MB/s (full_top)
        const int PRIORITIES[] = { -1, 0, 1, 2 };
        char g_lastResult[768] = "No recording yet.";

        void RefreshNames(void)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "Mode: %s", Recorder::ModeName(g_opt.mode));
            g_modeEntry->Name() = buf;
            g_sdEntry->Name() = g_opt.writeToSd ? "Write to SD: yes" : "Write to SD: no (capture only)";
            g_coreEntry->Name() = g_opt.writerCore == 0 ? "Writer core: 0 (app)" : "Writer core: 1 (sys)";
            snprintf(buf, sizeof(buf), "Write rate (write_only): %.1f MB/s", g_opt.writeRateKBs / 1024.0);
            g_rateEntry->Name() = buf;
            snprintf(buf, sizeof(buf), "Write block: %lu KB", g_opt.blockKB);
            g_blockEntry->Name() = buf;
            if (g_opt.filePriority < 0)
                snprintf(buf, sizeof(buf), "FS file priority: default");
            else
                snprintf(buf, sizeof(buf), "FS file priority: %d", g_opt.filePriority);
            g_prioEntry->Name() = buf;
            g_captureEntry->Name() = g_opt.useDma ? "Capture: DMA" : "Capture: CPU copy";
            g_codecEntry->Name() = g_opt.codec == 2 ? "Codec (DMA): XR32" : g_opt.codec == 1 ? "Codec (DMA): XOR-RLE" : "Codec (DMA): raw";
            g_convCoreEntry->Name() = g_opt.convertCore == 2 ? "Convert core (DMA): both (1 + 0)"
                                    : g_opt.convertCore == 1 ? "Convert core (DMA): 1 (sys)" : "Convert core (DMA): 0 (app, idle only)";
        }

        /* ------------------------------------------------------------ autotest */

        constexpr u32 AUTO_RUN_SECONDS = 45;
        // Pause between runs, nothing recorded: the next file is prepared meanwhile.
        constexpr u32 AUTO_GAP_SECONDS = 4;
        const Recorder::Mode AUTO_MODES[] = {
            Recorder::ModeCountOnly, Recorder::ModeHalfBoth, Recorder::ModeHybrid,
            Recorder::ModeFullTop, Recorder::ModeFullBoth,
        };
        constexpr u32 AUTO_COUNT = sizeof(AUTO_MODES) / sizeof(AUTO_MODES[0]);

        struct
        {
            bool active = false;
            bool needStart = false;
            bool needMicro = false;
            u32 index = 0;
            u64 startTick = 0;
            u64 gapUntil = 0;
            double baselineFps = 0;
            char table[768];
            int used = 0;
        } g_auto;

        void AutoAppend(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
        void AutoAppend(const char *fmt, ...)
        {
            if (g_auto.used >= (int)sizeof(g_auto.table) - 1)
                return;
            va_list ap;
            va_start(ap, fmt);
            int n = vsnprintf(g_auto.table + g_auto.used, sizeof(g_auto.table) - g_auto.used, fmt, ap);
            va_end(ap);
            if (n > 0)
                g_auto.used += n;
        }

        void AutoFinish(bool aborted)
        {
            g_auto.active = false;
            AutoAppend(aborted ? "Aborted.\n" : "Done. Details: game9_*.csv\n");
            snprintf(g_lastResult, sizeof(g_lastResult), "%s", g_auto.table);
            OSD::Notify(aborted ? "Autotest aborted" : "Autotest done: SELECT > Last result");
            if (!aborted)
                OSD::Notify("Then: HOME 10 s, back, SELECT > HOME probe");
        }

        void AutoRecordResult(void)
        {
            const Recorder::RunResult &r = Recorder::LastRun();
            if (r.mode == Recorder::ModeCountOnly)
            {
                g_auto.baselineFps = r.fpsSteady;
                AutoAppend("%-9s %4.1f fps (baseline, steady)\n", Recorder::ModeName(r.mode), r.fpsSteady);
                return;
            }
            AutoAppend("%-9s %4.1f %+4.1f lost %3.1f%% x%.2f %.2fMB/s", Recorder::ModeName(r.mode),
                       r.fpsSteady, r.fpsSteady - g_auto.baselineFps, r.dropPct, r.ratio, r.mbps);
            if (r.botSkipPct >= 0.5)
                AutoAppend(" bs%.0f%%", r.botSkipPct);
            if (r.topThrottlePct >= 0.5)
                AutoAppend(" ts%.0f%%", r.topThrottlePct);
            AutoAppend("\n");
        }

        /* Runs every frame from the hotkey game func. */
        void AutoTick(void)
        {
            if (!g_auto.active)
                return;
            char summary[768];
            if (g_auto.needMicro)
            {
                // Runs while the game plays (it pauses ~2 s): real consecutive frames for the encoder test.
                g_auto.needMicro = false;
                char mb[256];
                const Microbench::Report &m = Microbench::Run(mb, sizeof(mb));
                AutoAppend("%s", mb);
                if (m.valid)
                    g_opt.encVariant = m.bestVariant;
                g_auto.gapUntil = svcGetSystemTick() + (u64)AUTO_GAP_SECONDS * SYSCLOCK_ARM11;
                return;
            }
            if (g_auto.needStart)
            {
                if (svcGetSystemTick() < g_auto.gapUntil)
                    return;
                g_auto.needStart = false;
                Recorder::Options o = g_opt;
                o.mode = AUTO_MODES[g_auto.index];
                o.writeToSd = true;
                o.useDma = true;
                o.codec = 2;
                o.encVariant = g_opt.encVariant;
                o.keepRecording = o.mode == Recorder::ModeFullTop;   // codec research on the host
                char error[160] = "";
                if (!Recorder::Start(o, error, sizeof(error)))
                {
                    AutoAppend("%s: start failed: %s\n", Recorder::ModeName(o.mode), error);
                    AutoFinish(true);
                    return;
                }
                // No OSD::Notify here: v0.10 showed that drawing a notification
                // costs SM3DL ~1/3 of its frames for 5 s, which hid the real cost.
                g_auto.startTick = svcGetSystemTick();
                return;
            }
            const u64 elapsed = svcGetSystemTick() - g_auto.startTick;
            if (elapsed < (u64)AUTO_RUN_SECONDS * SYSCLOCK_ARM11)
                return;
            Recorder::Stop(summary, sizeof(summary));
            AutoRecordResult();
            if (++g_auto.index >= AUTO_COUNT)
                AutoFinish(false);
            else
            {
                g_auto.needStart = true;
                g_auto.gapUntil = svcGetSystemTick() + (u64)AUTO_GAP_SECONDS * SYSCLOCK_ARM11;
            }
        }

        void AutoMenuFunc(MenuEntry *)
        {
            if (Recorder::IsRunning() || g_auto.active)
            {
                MessageBox("A recording or autotest is already running.")();
                return;
            }
            g_auto.active = true;
            g_auto.needStart = true;
            g_auto.index = 0;
            g_auto.gapUntil = 0;
            g_auto.needMicro = true;
            g_auto.baselineFps = 0;
            g_auto.used = 0;
            g_auto.table[0] = 0;
            AutoAppend("v0.12 autotest %lu x %lu s, XR32, fps w/o first 6 s\n", AUTO_COUNT, AUTO_RUN_SECONDS);
            MessageBox("Autotest",
                       "Microbench (game freezes ~2 s), then\n5 runs x 45 s (~4.5 min): baseline, half_both,\n"
                       "hybrid, full_top, full_both.\n\n"
                       "Close the menu and keep playing the same scene.\n"
                       "Nothing is shown until 'Autotest done'.\nL+R+Down aborts.")();
        }

        void ToggleRecording(void)
        {
            if (Recorder::IsRunning())
            {
                Recorder::Stop(g_lastResult, sizeof(g_lastResult));
                OSD::Notify("Bench: stopped, results saved");
                return;
            }
            char error[160] = "";
            // No notification on start: drawing it slows the game for 5 s (v0.10).
            if (!Recorder::Start(g_opt, error, sizeof(error)))
            {
                snprintf(g_lastResult, sizeof(g_lastResult), "Start failed: %s", error);
                OSD::Notify(g_lastResult, Color::Red);
            }
        }

        /* Runs every frame while enabled: the global hotkey. */
        void HotkeyGameFunc(MenuEntry *)
        {
            if (Controller::IsKeysPressed(Key::L | Key::R | Key::DPadDown))
            {
                if (g_auto.active)
                {
                    if (Recorder::IsRunning())
                    {
                        char summary[768];
                        Recorder::Stop(summary, sizeof(summary));
                    }
                    AutoFinish(true);
                    return;
                }
                ToggleRecording();
                return;
            }
            AutoTick();
        }

        void RecordMenuFunc(MenuEntry *)
        {
            ToggleRecording();
            if (!Recorder::IsRunning())
                MessageBox("Last result", g_lastResult)();
        }

        void ModeMenuFunc(MenuEntry *)
        {
            if (Recorder::IsRunning())
            {
                MessageBox("Stop the recording first.")();
                return;
            }
            std::vector<std::string> options;
            options.push_back("count_only  (no capture, game fps only)");
            options.push_back("half_top    (200x120)");
            options.push_back("half_both   (200x120 + 160x120)");
            options.push_back("full_top    (400x240)");
            options.push_back("full_both   (400x240 + 320x240)");
            options.push_back("hybrid      (400x240 + 160x120)");
            options.push_back("write_only  (dummy data to SD, no capture)");
            Keyboard kb("Recording mode", options);
            int choice = kb.Open();
            if (choice >= 0 && choice < Recorder::ModeCount)
                g_opt.mode = (Recorder::Mode)choice;
            RefreshNames();
        }

        void SdMenuFunc(MenuEntry *)
        {
            if (!Recorder::IsRunning())
                g_opt.writeToSd = !g_opt.writeToSd;
            RefreshNames();
        }

        void CoreMenuFunc(MenuEntry *)
        {
            if (!Recorder::IsRunning())
                g_opt.writerCore = g_opt.writerCore == 0 ? 1 : 0;
            RefreshNames();
        }

        template <typename T, u32 N>
        T NextOf(const T (&values)[N], T current)
        {
            for (u32 i = 0; i < N; i++)
                if (values[i] == current)
                    return values[(i + 1) % N];
            return values[0];
        }

        void RateMenuFunc(MenuEntry *)
        {
            if (!Recorder::IsRunning())
                g_opt.writeRateKBs = NextOf(RATES_KBS, g_opt.writeRateKBs);
            RefreshNames();
        }

        void BlockMenuFunc(MenuEntry *)
        {
            if (!Recorder::IsRunning())
                g_opt.blockKB = g_opt.blockKB == 256 ? 64 : 256;
            RefreshNames();
        }

        void PrioMenuFunc(MenuEntry *)
        {
            if (!Recorder::IsRunning())
                g_opt.filePriority = NextOf(PRIORITIES, g_opt.filePriority);
            RefreshNames();
        }

        void CaptureMenuFunc(MenuEntry *)
        {
            if (!Recorder::IsRunning())
                g_opt.useDma = !g_opt.useDma;
            RefreshNames();
        }

        void CodecMenuFunc(MenuEntry *)
        {
            if (!Recorder::IsRunning())
                g_opt.codec = (g_opt.codec + 1) % 3;
            RefreshNames();
        }

        void ConvCoreMenuFunc(MenuEntry *)
        {
            if (!Recorder::IsRunning())
                g_opt.convertCore = (g_opt.convertCore + 1) % 3;
            RefreshNames();
        }

        void MemMenuFunc(MenuEntry *)
        {
            if (Recorder::IsRunning())
            {
                MessageBox("Stop the recording first.")();
                return;
            }
            char summary[512];
            RunMemProbe(summary, sizeof(summary));
            MessageBox("T3: plugin memory", summary)();
        }

        void DmaProbeMenuFunc(MenuEntry *)
        {
            if (Recorder::IsRunning())
            {
                MessageBox("Stop the recording first.")();
                return;
            }
            char summary[768];
            RunDmaProbe(summary, sizeof(summary));
            MessageBox("T4: DMA probe", summary)();
        }

        void MicroMenuFunc(MenuEntry *)
        {
            if (Recorder::IsRunning() || g_auto.active)
            {
                MessageBox("Stop the recording first.")();
                return;
            }
            char mb[256];
            const Microbench::Report &m = Microbench::Run(mb, sizeof(mb));
            if (m.valid)
                g_opt.encVariant = m.bestVariant;
            MessageBox("T5: microbench (menu open = frames identical)", mb)();
        }

        void HomeProbeMenuFunc(MenuEntry *)
        {
            char summary[512];
            HomeProbe::Report(summary, sizeof(summary));
            MessageBox("HOME probe (full-console recording)", summary)();
        }

        void ResultMenuFunc(MenuEntry *)
        {
            MessageBox("Last result", g_lastResult)();
        }
    }

    void PatchProcess(FwkSettings &settings)
    {
        settings.AllowActionReplay = false;
        settings.AllowSearchEngine = false;
    }

    void OnProcessExit(void)
    {
        if (Recorder::IsRunning())
            Recorder::Stop(g_lastResult, sizeof(g_lastResult));
        Recorder::Shutdown();
        HomeProbe::Stop();
        sdio_exit();
    }

    int main(void)
    {
        sdio_init();
        OSD::Run(DmaProbeTrackFramebuffer);

        Recorder::PrepareNext();
        HomeProbe::Start();

        PluginMenu *menu = new PluginMenu("Recorder bench", 0, 12, 0,
            "Phase 0 benchmarks: T2 (SD in game), T3 (memory), T4 (CPU capture), T8 (SD cost).\n"
            "Results: /3ds/recorder/bench/");
        menu->SynchronizeWithFrame(true);
        menu->ShowWelcomeMessage(false);

        MenuEntry *hotkey = new MenuEntry("Hotkey L+R+Down (keep enabled)", HotkeyGameFunc,
                                          "Starts / stops recording without opening the menu.");
        menu->Append(hotkey);
        hotkey->Enable();

        menu->Append(new MenuEntry("Autotest (5 x 45 s)", nullptr, AutoMenuFunc,
                                   "Baseline + 4 recording modes in a row. One table at the end."));
        menu->Append(new MenuEntry("Start / stop recording", nullptr, RecordMenuFunc,
                                   "Same as L+R+Down. Shows the result when stopped."));
        g_modeEntry = new MenuEntry("Mode", nullptr, ModeMenuFunc, "What to capture.");
        g_sdEntry = new MenuEntry("Write to SD", nullptr, SdMenuFunc,
                                  "'no' captures into RAM only: isolates capture cost from SD.");
        g_coreEntry = new MenuEntry("Writer core", nullptr, CoreMenuFunc,
                                    "Core for the SD writer thread. Falls back to 0 if 1 is denied.");
        menu->Append(g_modeEntry);
        g_captureEntry = new MenuEntry("Capture", nullptr, CaptureMenuFunc,
                                       "DMA: the game thread only starts a DMA copy. CPU: converts in the game thread.");
        g_convCoreEntry = new MenuEntry("Convert core", nullptr, ConvCoreMenuFunc,
                                        "DMA mode: core of the thread that converts frames. Falls back to 0.");
        menu->Append(g_captureEntry);
        menu->Append(g_convCoreEntry);
        g_codecEntry = new MenuEntry("Codec", nullptr, CodecMenuFunc,
                                     "DMA mode: raw RGB565, XOR-RLE or XR32 (pixel pairs, faster) vs the previous frame (lossless).");
        menu->Append(g_codecEntry);
        menu->Append(g_sdEntry);
        menu->Append(g_coreEntry);
        g_rateEntry = new MenuEntry("Write rate", nullptr, RateMenuFunc,
                                    "write_only mode: target SD rate. 1.2 / 2.6 / 5.8 MB/s.");
        g_blockEntry = new MenuEntry("Write block", nullptr, BlockMenuFunc,
                                     "Size of one SD write: 256 KB (T1 best) or 64 KB.");
        g_prioEntry = new MenuEntry("FS file priority", nullptr, PrioMenuFunc,
                                    "FSFILE_SetPriority on the recording file: default / 0 / 1 / 2.");
        menu->Append(g_rateEntry);
        menu->Append(g_blockEntry);
        menu->Append(g_prioEntry);
        menu->Append(new MenuEntry("T3: memory probe", nullptr, MemMenuFunc,
                                   "How much memory the plugin gets and whether it is contiguous."));
        menu->Append(new MenuEntry("T4: DMA probe", nullptr, DmaProbeMenuFunc,
                                   "Tries DMA source/destination combinations, shows which work."));
        menu->Append(new MenuEntry("HOME probe (full-console)", nullptr, HomeProbeMenuFunc,
                                   "Press HOME, wait 10 s, return, then open this: can a plugin see and copy HOME Menu frames?"));
        menu->Append(new MenuEntry("T5: microbench", nullptr, MicroMenuFunc,
                                   "Memory and encoder cost (the autotest runs it with real frames)."));
        menu->Append(new MenuEntry("Last result", nullptr, ResultMenuFunc));
        RefreshNames();

        menu->Run();
        // No `delete menu`: on power-off the CTRPF menu destructor crashed in
        // free() (crash_dump_00000016, PC in _free_r via ~PluginMenu). The
        // process is ending and Luma frees the plugin block anyway.
        return 0;
    }
}
