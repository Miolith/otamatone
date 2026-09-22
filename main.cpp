#include <cmath>
#include "raylib.h"

constexpr int SAMPLE_RATE = 44100;
constexpr float TAU = 6.28318f;
constexpr int BUFFER_SIZE = 1024;
const float SEMITONE = powf(2.0f, 1.0f / 12.0f);
constexpr int WINDOW_HEIGHT = 720;
constexpr int WINDOW_WIDTH = 920;
constexpr int PIXELS_PER_SEMITONE = 30;
constexpr float BASE_FREQUENCY = 110.0f;

const Color bgColor = GetColor(0x0D1117FF);
const Color noteStripeColor = GetColor(0x161B22FF);
const Color noteTextColor = GetColor(0xFFD1D9FF);
const Color guideYColor = GetColor(0xFF7B72FF);
const Color guideXColor = GetColor(0x58A6FFFF);
const Color accentTextColor = GetColor(0xA5D6FFFF);
const Color cursorColor = GetColor(0xFFB86CFF);

constexpr int whiteNotes[] = {0, 2, 3, 5, 7, 8, 10};
constexpr int blackNotes[] = {1, 4, 6, 9, 11};

#define to_f32(x) static_cast<float>(x)

static float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

struct BiquadResonator
{
    float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    void setCoefficients(float centerFreq, float bandwidth)
    {
        centerFreq = std::fmax(20.0f, std::fmin(centerFreq, SAMPLE_RATE * 0.49f));
        float w0 = TAU * centerFreq / SAMPLE_RATE;
        float Q  = centerFreq / bandwidth;
        float alphaVal = std::sin(w0) / (2.0f * Q);
        float cosw0 = std::cos(w0);
        float a0 = 1.0f + alphaVal;

        b0 = alphaVal / a0;
        b1 = 0.0f;
        b2 = -alphaVal / a0;
        a1 = -2.0f * cosw0 / a0;
        a2 = (1.0f - alphaVal) / a0;
    }

    float process(float in)
    {
        const float out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = in;
        y2 = y1; y1 = out;
        return out;
    }
};

static float square(float x)
{
    return fmodf(x, 1.0f) < 0.5f ? -1.0f : 1.0f;
}

static float sinewave(float x)
{
    return sinf(TAU * x);
}

static float sawtooth(float x)
{
    return fmodf(x, 1.0f) * 2.0f - 1.0f;
}

static float triangle(float x)
{
    return fabsf(fmodf(x, 1.0f) * 2.0f - 1.0f) * 2.0f - 1.0f;
}

static float noise(float x)
{
    return to_f32(rand()) / to_f32(RAND_MAX) * 2.0f - 1.0f;
}

static float fm(float x)
{
    float modulator = sinewave(x * 2.0f);
    return sinewave(x + modulator * 0.5f);
}
// Derivative of the Rosenberg glottal pulse. Phase normalized to [0, 1).
// Peak |value| is pi / (2·Tn) ~= 9.82, so we divide by 10 to keep amplitude
// near unity.
//
// `effort` (0..1) controls the pulse shape: 0 is lax/breathy (longer Tp,
// gentler closure), 1 is tense (shorter Tp, sharper closure)
static float glottalPulse(float x, float effort)
{
    const float e = effort < 0 ? 0 : effort > 1 ? 1 : effort;
    const float Tp = 0.5f - e * 0.2f;     // 0.5 (lax) -> 0.3 (tense)
    const float Tn = 0.25f - e * 0.17f;   // 0.25 (lax) -> 0.08 (tense)
    constexpr float norm = 0.1f;
    float phase = fmodf(x, 1.0f);
    if (phase < Tp) {
        return norm * 0.5f * (M_PI / Tp) * sinf(M_PI * phase / Tp);
    }
    if (phase < Tp + Tn) {
        return -norm * (M_PI / (2.0f * Tn)) * sinf(M_PI * (phase - Tp) / (2.0f * Tn));
    }
    return 0;
}

// Klatt synthesizer cascade vowel formant
struct PhonemeParams
{
    float f1, f2, f3; // formant frequencies
    float bw1, bw2, bw3; // formant bandwidths
    float a1, a2, a3; // formant amplitudes
};

// sp"a", f"a"ther,
constexpr PhonemeParams A = {
    700.0f, 1220.0f, 2600.0f,
    130.0f, 70.0f, 160.0f,
    1.0f, 0.9f, 0.7f
};

//g"o"
constexpr PhonemeParams O = {
    540.0f, 1100.0f, 2300.0f,
    80.0f, 70.0f, 70.0f,
    1.0f, 0.9f, 0.7f
};

static BiquadResonator f1, f2, f3;
static float targetF1, targetF2, targetF3;
static float targetBW1, targetBW2, targetBW3;

static float waveform(float x)
{
    float glottal = glottalPulse(x, 0.5f);

    f1.setCoefficients(targetF1, targetBW1);
    f2.setCoefficients(targetF2, targetBW2);
    f3.setCoefficients(targetF3, targetBW3);

    return f3.process(f2.process(f1.process(glottal))) * 400.0f;
}

int main()
{
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Audio Prog");
    InitAudioDevice();
    SetAudioStreamBufferSizeDefault(BUFFER_SIZE);
    float buffer[BUFFER_SIZE];
    AudioStream stream = LoadAudioStream(SAMPLE_RATE, 32, 1);

    SetAudioStreamPan(stream, 0.0f);
    PlayAudioStream(stream);

    SetTargetFPS(60);

    int sineIndex = 0;
    while (!WindowShouldClose())
    {
        int semitones = -1;
        if (IsKeyDown(KEY_Z) || IsKeyDown(KEY_X))
        {
            semitones = (WINDOW_HEIGHT - GetMouseY()) / PIXELS_PER_SEMITONE;
        }
        float vowelFactor = to_f32(GetMouseX()) / to_f32(WINDOW_WIDTH - 1);
        targetF1 = lerp(A.f1, O.f1, vowelFactor);
        targetF2 = lerp(A.f2, O.f2, vowelFactor);
        targetF3 = lerp(A.f3, O.f3, vowelFactor);
        targetBW1 = lerp(A.bw1, O.bw1, vowelFactor);
        targetBW2 = lerp(A.bw2, O.bw2, vowelFactor);
        targetBW3 = lerp(A.bw3, O.bw3, vowelFactor);

        if (IsAudioStreamProcessed(stream) && semitones != -1)
        {
            for (int i = 0; i < BUFFER_SIZE; i++)
            {
                float time = to_f32(sineIndex) / to_f32(SAMPLE_RATE);
                constexpr float volume = 4.0f;
                buffer[i] = waveform(
                    time * BASE_FREQUENCY * powf(SEMITONE, semitones)
                ) * volume;
                sineIndex++;
            }

            UpdateAudioStream(stream, buffer, BUFFER_SIZE);
        }
        BeginDrawing();
        ClearBackground(bgColor);

        for (int octave = 0; octave < 3; octave++)
        {
            for (int note = 0; note < 12; note += 2)
            {
                int y = WINDOW_HEIGHT - (note * PIXELS_PER_SEMITONE) - octave * 12 * PIXELS_PER_SEMITONE + PIXELS_PER_SEMITONE/2;
                DrawRectangle(0, y - PIXELS_PER_SEMITONE / 2, WINDOW_WIDTH, PIXELS_PER_SEMITONE, noteStripeColor);
            }
            for (char note = 0; note < 7; note++)
            {
                int y = WINDOW_HEIGHT - (whiteNotes[note] * PIXELS_PER_SEMITONE) - octave * 12 * PIXELS_PER_SEMITONE + PIXELS_PER_SEMITONE/2;
                char name[2]{};
                name[0] = 'A' + note;
                DrawText(name, 10, y - 10, 20, noteTextColor);
            }
        }

        DrawLine(0, GetMouseY(), WINDOW_WIDTH, GetMouseY(), guideYColor);
        DrawLine(GetMouseX(), 0, GetMouseX(), WINDOW_HEIGHT, guideXColor);
        DrawText("A", 150, WINDOW_HEIGHT - 25, 24, accentTextColor);
        DrawText("O", WINDOW_WIDTH - 150, WINDOW_HEIGHT - 25, 24, accentTextColor);

        float cursorRadius = IsKeyDown(KEY_Z) || IsKeyDown(KEY_X) ? 7 : 5;
        DrawCircle(GetMouseX(), GetMouseY(), cursorRadius, cursorColor);

        HideCursor();
        EndDrawing();
    }

    CloseAudioDevice();
    CloseWindow();
    return 0;
}