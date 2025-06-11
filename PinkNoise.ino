/*
  PinkNoisePlayer - M5Stack CoreS3 (v2.7)
  ----------------------------------------
  Features:
    - Plays PinkNoiseLong.wav from SD card (looped)
    - Reads WiFi credentials from /ssid.txt on SD card
    - Connects to WiFi and syncs time once via NTP (UK timezone)
    - Text output in RED (on black), smallest legible font
    - Buttons:
        A - Decrease volume
        B - Pause/Resume playback by setting volume to 0
        C - Increase volume

  SD Card Requirements:
    /PinkNoiseLong.wav  - 16-bit PCM WAV audio
    /ssid.txt           - Two lines: SSID and password
*/

#include <M5CoreS3.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <time.h>

#define SD_SPI_SCK_PIN  18
#define SD_SPI_MISO_PIN 19
#define SD_SPI_MOSI_PIN 23
#define SD_SPI_CS_PIN   4
#define WAV_PATH "/PinkNoiseLong.wav"

const char* tz = "GMT0BST,M3.5.0/1,M10.5.0/2";
String ssid = "", password = "";

File wavFile;
bool isPlaying = true;
int volume = 16;
int savedVolume = 16;
uint32_t sampleRate = 44100;
uint16_t bitsPerSample = 16;
uint8_t numChannels = 1;
bool speakerActive = true;

M5Canvas canvas(&M5.Display);

// Logging
void printf_log(const char *format, ...);
void println_log(const char *str);
void print_log(const char *str);

// Helpers
bool openWav(const char* path);
bool readWiFiCredentials();

void setup() {
    M5.begin();
    M5.Speaker.begin();
    M5.Speaker.setVolume(volume);

    canvas.setColorDepth(1);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    canvas.setPaletteColor(1, RED);  // 🔴 RED TEXT
    canvas.setTextSize(1.0);         // Smallest legible
    canvas.setTextScroll(true);
    canvas.fillSprite(BLACK);
    canvas.pushSprite(0, 0);

    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
    if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
        println_log("SD init failed.");
        while (1);
    }
    println_log("SD mounted.");

    if (!readWiFiCredentials()) {
        println_log("Read /ssid.txt failed.");
        while (1);
    }

    printf_log("Connecting to WiFi (%s)...\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), password.c_str());
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        print_log(".");
    }

    println_log("\nWiFi Connected!");
    printf_log("IP: %s\n", WiFi.localIP().toString().c_str());

    configTzTime(tz, "pool.ntp.org");
    println_log("Syncing time...");

    struct tm timeinfo;
    while (!getLocalTime(&timeinfo)) {
        delay(500);
        print_log("*");
    }

    println_log("\nTime synced.");

    if (!openWav(WAV_PATH)) {
        println_log("Failed to open WAV.");
        while (1);
    }

    println_log("Playing PinkNoiseLong.wav...");
}

void loop() {
    M5.update();
    static uint8_t buffer[512];

    // Mute state via volume = 0
    if (volume > 0 && isPlaying && speakerActive) {
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

    // Button A: Volume down
    if (M5.BtnA.wasPressed()) {
        volume = max(0, volume - 16);
        savedVolume = (volume > 0) ? volume : savedVolume;
        M5.Speaker.setVolume(volume);
        printf_log("Volume: %d\n", volume);
    }

    // Button B: Pause/resume (toggle volume)
    if (M5.BtnB.wasPressed()) {
        if (volume > 0) {
            savedVolume = volume;
            volume = 0;
            println_log("Paused");
        } else {
            volume = savedVolume;
            println_log("Resumed");
        }
        M5.Speaker.setVolume(volume);
    }

    // Button C: Volume up
    if (M5.BtnC.wasPressed()) {
        volume = min(255, volume + 16);
        savedVolume = volume;
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
    wavFile.read(); wavFile.read();  // skip audioFormat
    numChannels = wavFile.read() | (wavFile.read() << 8);
    sampleRate  = wavFile.read();
    sampleRate |= wavFile.read() << 8;
    sampleRate |= wavFile.read() << 16;
    sampleRate |= wavFile.read() << 24;

    wavFile.seek(34);
    bitsPerSample = wavFile.read() | (wavFile.read() << 8);
    wavFile.seek(44);  // PCM data start
    printf_log("WAV: %u ch, %u bits, %lu Hz\n", numChannels, bitsPerSample, sampleRate);
    return true;
}

bool readWiFiCredentials() {
    File file = SD.open("/ssid.txt");
    if (!file) {
        println_log("Failed to open /ssid.txt");
        return false;
    }

    String lines[2];
    int idx = 0;
    while (file.available() && idx < 2) {
        lines[idx++] = file.readStringUntil('\n');
    }
    file.close();

    ssid = lines[0]; ssid.trim();
    password = lines[1]; password.trim();
    return (ssid.length() > 0 && password.length() > 0);
}

// Logging functions
void println_log(const char *str) {
    Serial.println(str);
    canvas.println(str);
    canvas.pushSprite(0, 0);
}

void print_log(const char *str) {
    Serial.print(str);
    canvas.print(str);
    canvas.pushSprite(0, 0);
}

void printf_log(const char *format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    Serial.print(buf);
    canvas.printf(buf);
    canvas.pushSprite(0, 0);
}
