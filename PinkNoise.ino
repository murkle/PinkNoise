#include <M5CoreS3.h>
#include <SPI.h>
#include <SD.h>

#define SD_SPI_SCK_PIN  18
#define SD_SPI_MISO_PIN 19
#define SD_SPI_MOSI_PIN 23
#define SD_SPI_CS_PIN   4
#define WAV_PATH "/PinkNoiseLong.wav"

File wavFile;
bool isPlaying = true;
int volume = 16;
uint32_t sampleRate = 44100;
uint16_t bitsPerSample = 16;
uint8_t numChannels = 1;

M5Canvas canvas(&M5.Display);

void printf_log(const char *format, ...);
void println_log(const char *str);
bool openWav(const char* path);

void setup() {
    M5.begin();
    M5.Speaker.begin();
    M5.Speaker.setVolume(volume);

    canvas.setColorDepth(1);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    canvas.setPaletteColor(1, GREEN);
    canvas.setTextSize((float)canvas.width() / 160);
    canvas.setTextScroll(true);

    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);

    if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
        println_log("Card failed, or not present");
        while (1);
    }

    if (!openWav(WAV_PATH)) {
        println_log("Failed to open WAV.");
        while (1);
    }

    println_log("Playing /PinkNoise.wav in loop...");
}

void loop() {
    M5.update();
    static uint8_t buffer[512];

    if (isPlaying) {
        if (wavFile.available()) {
            size_t bytesRead = wavFile.read(buffer, sizeof(buffer));
            if (bytesRead > 0) {
                if (bitsPerSample == 8) {
                    M5.Speaker.playRaw(buffer, bytesRead, sampleRate, numChannels == 2, false);
                } else if (bitsPerSample == 16) {
                    M5.Speaker.playRaw((int16_t*)buffer, bytesRead / 2, sampleRate, numChannels == 2, false);
                }
            }
        } else {
            int16_t silence[128] = {0};
            M5.Speaker.playRaw(silence, 128, sampleRate, numChannels == 2, false);
            wavFile.seek(44);
        }
    }

    if (M5.BtnA.wasPressed()) {
        volume = max(0, volume - 16);
        M5.Speaker.setVolume(volume);
        printf_log("Volume: %d\n", volume);
    }

    if (M5.BtnB.wasPressed()) {
        isPlaying = !isPlaying;
        println_log(isPlaying ? "Playing" : "Paused");
    }

    if (M5.BtnC.wasPressed()) {
        volume = min(255, volume + 16);
        M5.Speaker.setVolume(volume);
        printf_log("Volume: %d\n", volume);
    }
}

bool openWav(const char* path) {
    wavFile = SD.open(path);
    if (!wavFile) return false;

    char riff[4];
    wavFile.readBytes(riff, 4);
    if (strncmp(riff, "RIFF", 4) != 0) return false;

    wavFile.seek(20);
    uint16_t audioFormat = wavFile.read() | (wavFile.read() << 8);
    numChannels = wavFile.read() | (wavFile.read() << 8);
    sampleRate  = wavFile.read() | (wavFile.read() << 8) | (wavFile.read() << 16) | (wavFile.read() << 24);

    wavFile.seek(34);
    bitsPerSample = wavFile.read() | (wavFile.read() << 8);

    wavFile.seek(44);
    printf_log("WAV: %u ch, %u bits, %lu Hz\n", numChannels, bitsPerSample, sampleRate);
    return true;
}

void printf_log(const char *format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, 256, format, args);
    va_end(args);
    Serial.print(buf);
    canvas.printf(buf);
    canvas.pushSprite(0, 0);
}

void println_log(const char *str) {
    Serial.println(str);
    canvas.println(str);
    canvas.pushSprite(0, 0);
}
