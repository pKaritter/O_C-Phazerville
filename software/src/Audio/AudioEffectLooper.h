#pragma once

#include <Arduino.h>
#include <AudioStream.h>
#include <arm_math.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" uint8_t external_psram_size;
extern "C" void* extmem_malloc(size_t);
extern "C" void extmem_free(void* ptr);

enum LooperState : uint8_t {
    LOOPER_EMPTY = 0,
    LOOPER_REC,
    LOOPER_PLAY,
    LOOPER_OVERDUB,
    LOOPER_STOP
};

template <int Channels>
class AudioEffectLooper : public AudioStream {
public:
    AudioEffectLooper() : AudioStream(Channels, inputQueueArray) {}

    ~AudioEffectLooper() {
        end();
    }

    bool begin() {
        if (buffer) return true;

        size_t alloc_bytes = 0;
        if (external_psram_size > 0) {
            // 4 MB in PSRAM -> ~43.6s mono / ~21.8s stereo at 48 kHz
            alloc_bytes = 4 * 1024 * 1024;
            buffer = static_cast<int16_t*>(extmem_malloc(alloc_bytes));
            using_psram = (buffer != nullptr);
        }
        if (!buffer) {
            // Fallback to RAM2 heap: 128 KB -> ~1.36s mono / ~0.68s stereo
            alloc_bytes = 128 * 1024;
            buffer = static_cast<int16_t*>(malloc(alloc_bytes));
            using_psram = false;
        }
        if (!buffer) {
            max_frames = 0;
            return false;
        }

        max_frames = (uint32_t)(alloc_bytes / (sizeof(int16_t) * Channels));
        clear();
        return true;
    }

    void end() {
        AudioNoInterrupts();
        int16_t* ptr = buffer;
        bool was_psram = using_psram;
        buffer = nullptr;
        max_frames = 0;
        loop_frames = 0;
        play_pos = 0.0f;
        rec_pos = 0;
        state = LOOPER_EMPTY;
        AudioInterrupts();

        if (ptr) {
            if (was_psram) extmem_free(ptr);
            else free(ptr);
        }
    }

    void clear() {
        AudioNoInterrupts();
        state = LOOPER_EMPTY;
        loop_frames = 0;
        rec_pos = 0;
        play_pos = 0.0f;
        AudioInterrupts();
    }

    void startRecord() {
        if (!buffer || max_frames < 256) return;
        AudioNoInterrupts();
        rec_pos = 0;
        loop_frames = 0;
        play_pos = 0.0f;
        state = LOOPER_REC;
        AudioInterrupts();
    }

    void finishRecordAndPlay() {
        if (!buffer) return;
        AudioNoInterrupts();
        if (state == LOOPER_REC) {
            if (rec_pos < 256) {
                state = LOOPER_EMPTY;
                loop_frames = 0;
                rec_pos = 0;
                play_pos = 0.0f;
            } else {
                loop_frames = rec_pos;
                applySeamCrossfade();
                play_pos = 0.0f;
                state = LOOPER_PLAY;
            }
        }
        AudioInterrupts();
    }

    void setState(LooperState new_state) {
        if (!buffer) return;
        if (new_state == LOOPER_REC) {
            startRecord();
            return;
        }
        AudioNoInterrupts();
        if (state == LOOPER_REC && (new_state == LOOPER_PLAY || new_state == LOOPER_OVERDUB)) {
            if (rec_pos < 256) {
                state = LOOPER_EMPTY;
                loop_frames = 0;
                rec_pos = 0;
                play_pos = 0.0f;
            } else {
                loop_frames = rec_pos;
                applySeamCrossfade();
                play_pos = 0.0f;
                state = new_state;
            }
        } else if (loop_frames > 0) {
            if (state == LOOPER_STOP && (new_state == LOOPER_PLAY || new_state == LOOPER_OVERDUB)) {
                play_pos = 0.0f;
            }
            state = new_state;
        } else {
            state = LOOPER_EMPTY;
        }
        AudioInterrupts();
    }

    LooperState getState() const { return state; }
    uint32_t getLoopFrames() const { return loop_frames; }
    uint32_t getMaxFrames() const { return max_frames; }
    bool isUsingPSRAM() const { return using_psram; }

    float getProgress() const {
        if (state == LOOPER_REC) {
            return (max_frames > 0) ? ((float)rec_pos / (float)max_frames) : 0.0f;
        }
        if (loop_frames > 0) {
            float p = play_pos / (float)loop_frames;
            return constrain(p, 0.0f, 1.0f);
        }
        return 0.0f;
    }

    void setFeedback(float fb) {
        feedback = constrain(fb, 0.0f, 1.0f);
    }

    void setMix(float m) {
        mix = constrain(m, 0.0f, 1.0f);
    }

    void setSpeed(float s) {
        speed = constrain(s, -2.0f, 2.0f);
    }

    void update() override {
        audio_block_t* blocks[Channels];
        for (int ch = 0; ch < Channels; ++ch) {
            blocks[ch] = receiveWritable(ch);
            if (!blocks[ch]) {
                blocks[ch] = allocate();
                if (blocks[ch]) memset(blocks[ch]->data, 0, sizeof(blocks[ch]->data));
            }
        }

        if (!buffer || max_frames == 0) {
            for (int ch = 0; ch < Channels; ++ch) {
                if (blocks[ch]) {
                    transmit(blocks[ch], ch);
                    release(blocks[ch]);
                }
            }
            return;
        }

        // DJ-style looper mix: 0..0.5 brings loop up with dry at 1.0; 0.5..1.0 fades dry out with loop at 1.0
        const float dry_gain = (mix <= 0.5f) ? 1.0f : (2.0f * (1.0f - mix));
        const float wet_gain = (mix <= 0.5f) ? (2.0f * mix) : 1.0f;
        const float fb = feedback;
        const float spd = speed;

        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            if (state == LOOPER_REC) {
                for (int ch = 0; ch < Channels; ++ch) {
                    int16_t in_s = blocks[ch] ? blocks[ch]->data[i] : 0;
                    buffer[rec_pos * Channels + ch] = in_s;
                    if (blocks[ch]) {
                        blocks[ch]->data[i] = (int16_t)__SSAT((int32_t)(in_s * dry_gain), 16);
                    }
                }
                rec_pos++;
                if (rec_pos >= max_frames) {
                    loop_frames = max_frames;
                    applySeamCrossfade();
                    play_pos = 0.0f;
                    state = LOOPER_PLAY;
                }
            } else if ((state == LOOPER_PLAY || state == LOOPER_OVERDUB) && loop_frames > 0) {
                // Wrap play_pos into [0, loop_frames)
                while (play_pos >= (float)loop_frames) play_pos -= (float)loop_frames;
                while (play_pos < 0.0f) play_pos += (float)loop_frames;

                uint32_t idx0 = (uint32_t)play_pos;
                uint32_t idx1 = idx0 + 1;
                if (idx1 >= loop_frames) idx1 = 0;
                float frac = play_pos - (float)idx0;

                for (int ch = 0; ch < Channels; ++ch) {
                    int16_t in_s = blocks[ch] ? blocks[ch]->data[i] : 0;
                    float s0 = (float)buffer[idx0 * Channels + ch];
                    float s1 = (float)buffer[idx1 * Channels + ch];
                    float loop_s = s0 + frac * (s1 - s0);

                    if (state == LOOPER_OVERDUB) {
                        float written = loop_s * fb + (float)in_s;
                        // Gentle saturation to prevent harsh wrap-around during heavy layering
                        if (written > 32767.0f) written = 32767.0f;
                        else if (written < -32768.0f) written = -32768.0f;
                        buffer[idx0 * Channels + ch] = (int16_t)written;
                    }

                    if (blocks[ch]) {
                        float out_f = (float)in_s * dry_gain + loop_s * wet_gain;
                        blocks[ch]->data[i] = (int16_t)__SSAT((int32_t)out_f, 16);
                    }
                }

                play_pos += spd;
            } else {
                // EMPTY or STOP: pass dry signal through
                for (int ch = 0; ch < Channels; ++ch) {
                    if (blocks[ch]) {
                        int16_t in_s = blocks[ch]->data[i];
                        blocks[ch]->data[i] = (int16_t)__SSAT((int32_t)(in_s * dry_gain), 16);
                    }
                }
            }
        }

        for (int ch = 0; ch < Channels; ++ch) {
            if (blocks[ch]) {
                transmit(blocks[ch], ch);
                release(blocks[ch]);
            }
        }
    }

private:
    void applySeamCrossfade() {
        // Short linear ramp at loop start & end (~2.7ms) to eliminate loop seam clicks
        const uint32_t fade = (loop_frames > 256) ? 128 : (loop_frames / 4);
        if (fade < 4) return;
        for (uint32_t i = 0; i < fade; ++i) {
            float g = (float)i / (float)fade;
            uint32_t end_idx = loop_frames - 1 - i;
            for (int ch = 0; ch < Channels; ++ch) {
                buffer[i * Channels + ch] = (int16_t)((float)buffer[i * Channels + ch] * g);
                buffer[end_idx * Channels + ch] = (int16_t)((float)buffer[end_idx * Channels + ch] * g);
            }
        }
    }

    audio_block_t* inputQueueArray[Channels];
    int16_t* buffer = nullptr;
    uint32_t max_frames = 0;
    uint32_t loop_frames = 0;
    uint32_t rec_pos = 0;
    float play_pos = 0.0f;

    volatile LooperState state = LOOPER_EMPTY;
    bool using_psram = false;

    float feedback = 1.0f;
    float mix = 0.5f;
    float speed = 1.0f;
};
