#pragma once

// ============================================================================
// LooperApplet — Real-Time Audio Looper with Overdub, Varispeed & Clock Sync
// ============================================================================
//
// Features:
//   - Mono (~43s in PSRAM) and Stereo (~21s in PSRAM) real-time looping
//     (with automatic RAM2 fallback micro-looper if no PSRAM is present)
//   - One-button / Trigger workflow: EMPTY -> REC -> PLAY <-> OVERDUB
//   - Clock-quantized recording & loop closing (Free, Beat, 1Bar, 2Bar, 4Bar)
//   - Overdub Feedback/Decay (Fdb: 0..100%) for infinite layering or tape decay
//   - DJ-style Dry/Loop Mix (0% dry, 50% full dry + full loop, 100% loop only)
//   - Real-time Varispeed & Reverse (-2x, -1x, -0.5x, +0.5x, +1x, +2x)
//   - Dual additive CVInputMap modulation on Feedback, Mix, and Speed
//   - Live playhead progress bar + Clear / Stop controls
// ============================================================================

#include "../Audio/AudioEffectLooper.h"

static constexpr float looper_speed_values[] = {
    -2.0f, -1.0f, -0.5f, 0.5f, 1.0f, 2.0f
};
static const char* const looper_speed_names[] = {
    "-2x ", "-1x ", "-.5x", "+.5x", "+1x ", "+2x "
};
static constexpr int NUM_LOOPER_SPEEDS = sizeof(looper_speed_values) / sizeof(looper_speed_values[0]);

static const char* const looper_sync_names[] = {
    "Free", "Beat", "1Bar", "2Bar", "4Bar"
};
static constexpr int NUM_LOOPER_SYNCS = sizeof(looper_sync_names) / sizeof(looper_sync_names[0]);

template <AudioChannels Channels>
class LooperApplet : public HemisphereAudioApplet {
public:
    const char* applet_name() {
        return "Looper";
    }

    void Start() override {
        alloc_ok = looper.begin();
        for (int ch = 0; ch < Channels; ++ch) {
            PatchCable(input, ch, looper, ch);
            PatchCable(looper, ch, output, ch);
        }
    }

    void Unload() override {
        looper.end();
        alloc_ok = false;
        armed_action = ARM_NONE;
        AllowRestart();
    }

    void Reset() override {
        rec_cv.Reset();
    }

    void Controller() override {
        if (!alloc_ok) return;

        if (rec_cv.Clock()) {
            RequestTransportStep();
        }

        // Handle clock-quantized arming (Beat / 1Bar / 2Bar / 4Bar)
        bool clock_running = HS::clock_m.IsRunning();
        if (sync_mode > 0 && clock_running) {
            if (HS::clock_m.EndOfBeat()) {
                beat_counter++;
                bool boundary = false;
                if (sync_mode == 1) boundary = true;                       // Beat
                else if (sync_mode == 2) boundary = (beat_counter % 4 == 0);  // 1 Bar
                else if (sync_mode == 3) boundary = (beat_counter % 8 == 0);  // 2 Bars
                else if (sync_mode == 4) boundary = (beat_counter % 16 == 0); // 4 Bars

                if (boundary && armed_action != ARM_NONE) {
                    ExecuteArmedAction();
                }
            }
        } else if (armed_action != ARM_NONE) {
            // If clock isn't running or Free mode, execute immediately
            ExecuteArmedAction();
        }

        if (!clock_running && was_clock_running) {
            beat_counter = 0;
        }
        was_clock_running = clock_running;

        // Dual additive modulation for Feedback, Mix, and Speed
        float eff_fdb = constrain(0.01f * feedback + fdb_cv.InF() + fdb_cv2.InF(), 0.0f, 1.0f);
        float eff_mix = constrain(0.01f * mix + mix_cv.InF() + mix_cv2.InF(), 0.0f, 1.0f);
        float base_spd = looper_speed_values[constrain((int)speed_idx, 0, NUM_LOOPER_SPEEDS - 1)];
        float eff_spd = constrain(base_spd + (spd_cv.InF() + spd_cv2.InF()) * 2.0f, -2.0f, 2.0f);

        looper.setFeedback(eff_fdb);
        looper.setMix(eff_mix);
        looper.setSpeed(eff_spd);
    }

    void View() override {
        if (!alloc_ok) {
            gfxPrint(1, 25, "Out of RAM!");
            return;
        }

        if (looper.isUsingPSRAM()) {
            gfxPrint(56, 2, "+");
        }

        LooperState st = looper.getState();

        // Row 1 (y=13): State badge + Rec Trigger Input
        const char* st_str = "EMPT";
        bool inv = false;
        if (armed_action != ARM_NONE) {
            st_str = "ARM ";
            inv = CursorBlink();
        } else {
            switch (st) {
                case LOOPER_EMPTY:   st_str = "EMPT"; break;
                case LOOPER_REC:     st_str = "REC "; inv = true; break;
                case LOOPER_PLAY:    st_str = "PLAY"; break;
                case LOOPER_OVERDUB: st_str = "ODUB"; inv = true; break;
                case LOOPER_STOP:    st_str = "STOP"; break;
            }
        }
        gfxPrint(1, 13, st_str);
        if (inv) gfxInvert(0, 12, 26, 9);

        gfxPrint(30, 13, "Tr:");
        gfxStartCursor(48, 13);
        gfxPrint(rec_cv);
        gfxEndCursor(cursor == CURSOR_REC_CV, false, rec_cv.InputName(), "RecTrig");

        // Playhead / Record progress bar (y=21, 62px wide)
        gfxFrame(1, 22, 62, 3);
        float prog = looper.getProgress();
        if (prog > 0.0f && st != LOOPER_EMPTY) {
            int bar_w = constrain((int)(prog * 60.0f), 1, 60);
            graphics.drawHLine(2, 23, bar_w);
        }

        // Row 2 (y=27): Feedback + Dual CV
        gfxPrint(1, 27, "Fdb");
        gfxStartCursor(21, 27);
        graphics.printf("%3d%%", feedback);
        gfxEndCursor(cursor == CURSOR_FEEDBACK);
        gfxStartCursor(46, 27);
        gfxPrint(fdb_cv);
        gfxEndCursor(cursor == CURSOR_FDB_CV, false, fdb_cv.InputName());
        gfxStartCursor(54, 27);
        gfxPrint(fdb_cv2);
        gfxEndCursor(cursor == CURSOR_FDB_CV2, false, fdb_cv2.InputName());

        // Row 3 (y=37): Mix + Dual CV
        gfxPrint(1, 37, "Mix");
        gfxStartCursor(21, 37);
        graphics.printf("%3d%%", mix);
        gfxEndCursor(cursor == CURSOR_MIX);
        gfxStartCursor(46, 37);
        gfxPrint(mix_cv);
        gfxEndCursor(cursor == CURSOR_MIX_CV, false, mix_cv.InputName());
        gfxStartCursor(54, 37);
        gfxPrint(mix_cv2);
        gfxEndCursor(cursor == CURSOR_MIX_CV2, false, mix_cv2.InputName());

        // Row 4 (y=47): Speed / Reverse + Dual CV
        gfxPrint(1, 47, "Spd");
        gfxStartCursor(21, 47);
        graphics.print(looper_speed_names[constrain((int)speed_idx, 0, NUM_LOOPER_SPEEDS - 1)]);
        gfxEndCursor(cursor == CURSOR_SPEED);
        gfxStartCursor(46, 47);
        gfxPrint(spd_cv);
        gfxEndCursor(cursor == CURSOR_SPD_CV, false, spd_cv.InputName());
        gfxStartCursor(54, 47);
        gfxPrint(spd_cv2);
        gfxEndCursor(cursor == CURSOR_SPD_CV2, false, spd_cv2.InputName());

        // Row 5 (y=56): Sync mode + [STP]/[PLY] + [CLR]
        gfxStartCursor(1, 56);
        graphics.print(looper_sync_names[constrain((int)sync_mode, 0, NUM_LOOPER_SYNCS - 1)]);
        gfxEndCursor(cursor == CURSOR_SYNC);

        gfxStartCursor(28, 56);
        graphics.print(st == LOOPER_STOP ? "PLY" : "STP");
        gfxEndCursor(cursor == CURSOR_STOP);

        gfxStartCursor(48, 56);
        graphics.print("CLR");
        gfxEndCursor(cursor == CURSOR_CLEAR);

        gfxDisplayInputMapEditor();
    }

    // Aux Button: Main one-touch Looper transport (REC -> PLAY <-> OVERDUB)
    void AuxButton() override {
        RequestTransportStep();
        CancelEdit();
    }

    void OnButtonPress() override {
        if (CheckEditInputMapPress(
            cursor,
            IndexedInput(CURSOR_REC_CV,  rec_cv),
            IndexedInput(CURSOR_FDB_CV,  fdb_cv),
            IndexedInput(CURSOR_FDB_CV2, fdb_cv2),
            IndexedInput(CURSOR_MIX_CV,  mix_cv),
            IndexedInput(CURSOR_MIX_CV2, mix_cv2),
            IndexedInput(CURSOR_SPD_CV,  spd_cv),
            IndexedInput(CURSOR_SPD_CV2, spd_cv2)
        )) return;

        if (cursor == CURSOR_STOP) {
            LooperState st = looper.getState();
            armed_action = ARM_NONE;
            if (st == LOOPER_STOP) looper.setState(LOOPER_PLAY);
            else if (st != LOOPER_EMPTY) looper.setState(LOOPER_STOP);
        } else if (cursor == CURSOR_CLEAR) {
            armed_action = ARM_NONE;
            looper.clear();
        } else {
            CursorToggle();
        }
    }

    void OnEncoderMove(int direction) override {
        if (!EditMode()) {
            MoveCursor(cursor, direction, CURSOR_COUNT - 1);
            return;
        }

        if (EditSelectedInputMap(direction)) return;

        switch (cursor) {
            case CURSOR_REC_CV:
                rec_cv.ChangeSource(direction);
                break;
            case CURSOR_FEEDBACK:
                feedback = (uint8_t)constrain((int)feedback + direction, 0, 100);
                break;
            case CURSOR_FDB_CV:
                fdb_cv.ChangeSource(direction);
                break;
            case CURSOR_FDB_CV2:
                fdb_cv2.ChangeSource(direction);
                break;
            case CURSOR_MIX:
                mix = (uint8_t)constrain((int)mix + direction, 0, 100);
                break;
            case CURSOR_MIX_CV:
                mix_cv.ChangeSource(direction);
                break;
            case CURSOR_MIX_CV2:
                mix_cv2.ChangeSource(direction);
                break;
            case CURSOR_SPEED:
                speed_idx = (uint8_t)constrain((int)speed_idx + direction, 0, NUM_LOOPER_SPEEDS - 1);
                break;
            case CURSOR_SPD_CV:
                spd_cv.ChangeSource(direction);
                break;
            case CURSOR_SPD_CV2:
                spd_cv2.ChangeSource(direction);
                break;
            case CURSOR_SYNC:
                sync_mode = (uint8_t)constrain((int)sync_mode + direction, 0, NUM_LOOPER_SYNCS - 1);
                break;
            default:
                break;
        }
    }

    void OnDataRequest(std::array<uint64_t, CONFIG_SIZE>& data) override {
        data[0] = PackPackables(
            pack<8>(feedback),
            pack<8>(mix),
            pack<4>(speed_idx),
            pack<4>(sync_mode)
        );
        data[1] = PackPackables(rec_cv, fdb_cv, fdb_cv2);
        data[2] = PackPackables(mix_cv, mix_cv2, spd_cv, spd_cv2);
    }

    void OnDataReceive(const std::array<uint64_t, CONFIG_SIZE>& data) override {
        if (data[0] == 0 && data[1] == 0 && data[2] == 0) {
            feedback = 100;
            mix = 50;
            speed_idx = 4; // +1x
            sync_mode = 0; // Free
            return;
        }
        UnpackPackables(
            data[0],
            pack<8>(feedback),
            pack<8>(mix),
            pack<4>(speed_idx),
            pack<4>(sync_mode)
        );
        UnpackPackables(data[1], rec_cv, fdb_cv, fdb_cv2);
        UnpackPackables(data[2], mix_cv, mix_cv2, spd_cv, spd_cv2);
        feedback = (uint8_t)constrain((int)feedback, 0, 100);
        mix = (uint8_t)constrain((int)mix, 0, 100);
        speed_idx = (uint8_t)constrain((int)speed_idx, 0, NUM_LOOPER_SPEEDS - 1);
        sync_mode = (uint8_t)constrain((int)sync_mode, 0, NUM_LOOPER_SYNCS - 1);
    }

    AudioStream* InputStream() override { return &input; }
    AudioStream* OutputStream() override { return &output; }

protected:
    void SetHelp() override {
        help[HELP_EXTRA1] = "Aux/Tr: Rec/Dub";
        help[HELP_EXTRA2] = "Sync: Free/Bars";
    }

private:
    enum Cursor {
        CURSOR_REC_CV = 0,
        CURSOR_FEEDBACK,
        CURSOR_FDB_CV,
        CURSOR_FDB_CV2,
        CURSOR_MIX,
        CURSOR_MIX_CV,
        CURSOR_MIX_CV2,
        CURSOR_SPEED,
        CURSOR_SPD_CV,
        CURSOR_SPD_CV2,
        CURSOR_SYNC,
        CURSOR_STOP,
        CURSOR_CLEAR,
        CURSOR_COUNT
    };

    enum ArmedAction : uint8_t {
        ARM_NONE = 0,
        ARM_REC,
        ARM_PLAY
    };

    void RequestTransportStep() {
        if (!alloc_ok) return;
        LooperState st = looper.getState();
        bool quantize = (sync_mode > 0 && HS::clock_m.IsRunning());

        if (st == LOOPER_EMPTY) {
            if (quantize) armed_action = ARM_REC;
            else looper.startRecord();
        } else if (st == LOOPER_REC) {
            if (quantize) armed_action = ARM_PLAY;
            else looper.finishRecordAndPlay();
        } else if (st == LOOPER_PLAY) {
            looper.setState(LOOPER_OVERDUB);
        } else if (st == LOOPER_OVERDUB) {
            looper.setState(LOOPER_PLAY);
        } else if (st == LOOPER_STOP) {
            looper.setState(LOOPER_PLAY);
        }
    }

    void ExecuteArmedAction() {
        ArmedAction act = armed_action;
        armed_action = ARM_NONE;
        if (act == ARM_REC) {
            looper.startRecord();
        } else if (act == ARM_PLAY) {
            looper.finishRecordAndPlay();
        }
    }

    int8_t cursor = 0;
    uint8_t feedback = 100;
    uint8_t mix = 50;
    uint8_t speed_idx = 4; // +1x
    uint8_t sync_mode = 0; // Free
    bool alloc_ok = false;

    ArmedAction armed_action = ARM_NONE;
    uint32_t beat_counter = 0;
    bool was_clock_running = false;

    DigitalInputMap rec_cv;
    CVInputMap fdb_cv;
    CVInputMap fdb_cv2;
    CVInputMap mix_cv;
    CVInputMap mix_cv2;
    CVInputMap spd_cv;
    CVInputMap spd_cv2;

    AudioPassthrough<Channels> input;
    AudioEffectLooper<Channels> looper;
    AudioPassthrough<Channels> output;
};
