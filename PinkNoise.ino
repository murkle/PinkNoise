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
uint32_t sampleRate = 44100;
uint16_t bitsPerSample = 16;
uint8_t numChannels = 1;
bool muteCleared = false;

M5Canvas canvas(&M5.Display);

// Logging helpers
void printf_log(const char *format, ...);
void println_log(const char *str);
void print_log(const char *str);

bool openWav(const char* path);
bool readWiFiCredentials();
void showCurrentTime();

void setup() {
    M5.begin();
    M5.Speaker.begin();
    M5.Speaker.setVolume(volume);

    canvas.setColorDepth(1);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    canvas.setPaletteColor(1, GREEN);
    canvas.setTextSize((float)canvas.width() / 160);
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

    println_log("Connecting to WiFi...");
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

    println_log("Playing PinkNoiseLong.wav in loop...");
}

unsigned long lastTimeDisplay = 0;

void loop() {
    M5.update();
    static uint8_t buffer[512];

    if (volume > 0 && isPlaying) {
        if (wavFile.available()) {
            size_t bytesRead = wavFile.read(buffer, sizeof(buffer));
            if (bytesRead > 0) {
                if (bitsPerSample == 8) {
                    M5.Speaker.playRaw(buffer, bytesRead, sampleRate, numChannels == 2, false);
                } else if (bitsPerSample == 16) {
                    M5.Speaker.playRaw((int16_t*)buffer, bytesRead / 2, sampleRate, numChannels == 2, false);
                }
            }
            muteCleared = false;
        } else {
            int16_t silence[128] = {0};
            M5.Speaker.playRaw(silence, 128, sampleRate, numChannels == 2, false);
            wavFile.seek(44);
        }
    } else if (!muteCleared) {
        int16_t silence[128] = {0};
        M5.Speaker.playRaw(silence, 128, sampleRate, numChannels == 2, false);
        muteCleared = true;
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

    if (millis() - lastTimeDisplay > 10000) {
        showCurrentTime();
        lastTimeDisplay = millis();
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

    wavFile.seek(44);  // PCM data starts here
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

void showCurrentTime() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        char timeStr[32];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
        printf_log("Time: %s (%s)\n", timeStr, timeinfo.tm_isdst ? "BST" : "GMT");
    }
}

// Green log output with newline
void println_log(const char *str) {
    Serial.println(str);
    canvas.println(str);
    canvas.pushSprite(0, 0);
}

// Green log output without newline
void print_log(const char *str) {
    Serial.print(str);
    canvas.print(str);
    canvas.pushSprite(0, 0);
}

// Green log formatted print
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
