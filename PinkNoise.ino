#include <M5CoreS3.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <time.h>
#include <vector>

#define SD_SPI_SCK_PIN  18
#define SD_SPI_MISO_PIN 19
#define SD_SPI_MOSI_PIN 23
#define SD_SPI_CS_PIN   4
#define WAV_PATH "/PinkNoiseLong.wav"
#define SCHEDULE_PATH "/schedule.txt"

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
bool wasPlayingLastLoop = false;
bool timeSyncedAfterStop = false;
int lastLoggedVolume = -1;

M5Canvas canvas(&M5.Display);

struct TimeRange {
    int startMin;
    int endMin;
    uint8_t volume;
};

std::vector<TimeRange> schedule;

void printf_log(const char *format, ...);
void println_log(const char *str);
void print_log(const char *str);
bool openWav(const char* path);
bool readWiFiCredentials();
void showCurrentTimeOnce();
bool loadSchedule(const char* path);
int getScheduledVolume();

void setup() {
    M5.begin();
    M5.Speaker.begin();
    M5.Speaker.setVolume(volume);

    canvas.setColorDepth(1);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    canvas.setPaletteColor(1, RED);
    canvas.setTextSize(1.0);
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
    showCurrentTimeOnce();

    if (!openWav(WAV_PATH)) {
        println_log("Failed to open WAV.");
        while (1);
    }

    if (!loadSchedule(SCHEDULE_PATH)) {
        println_log("No valid schedule. Playing always.");
    } else {
        println_log("Schedule loaded.");
    }

    println_log("Ready.");
}

void loop() {
    M5.update();
    static uint8_t buffer[512];
    int scheduledVol = getScheduledVolume();
    int effectiveVolume = (scheduledVol >= 0) ? scheduledVol : volume;
    bool shouldPlay = (effectiveVolume > 0 && isPlaying && speakerActive);

    if (scheduledVol >= 0 && scheduledVol != lastLoggedVolume) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            char buf[32];
            strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
            printf_log("%s Volume: %d\n", buf, scheduledVol);
        } else {
            printf_log("Volume changed to %d\n", scheduledVol);
        }
        lastLoggedVolume = scheduledVol;
    }

    if (shouldPlay) {
        M5.Speaker.setVolume(effectiveVolume);
        if (wavFile.available()) {
            size_t bytesRead = wavFile.read(buffer, sizeof(buffer));
            if (bytesRead > 0) {
                M5.Speaker.playRaw((int16_t*)buffer, bytesRead / 2, sampleRate, numChannels == 2, false);
            }
        } else {
            wavFile.seek(44); // rewind
        }
    } else {
        M5.Speaker.stop(); // stop all sound if not playing
    }

    if (!shouldPlay && !timeSyncedAfterStop) {
        configTzTime(tz, "pool.ntp.org");
        struct tm t;
        while (!getLocalTime(&t)) {
            delay(200);
        }
        println_log("Time re-synced after volume 0 or stop.");
        char timeStr[32];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M", &t);
        printf_log("Time: %s (%s)\n", timeStr, t.tm_isdst ? "BST" : "GMT");
        timeSyncedAfterStop = true;
    }

    if (shouldPlay && !wasPlayingLastLoop) {
        timeSyncedAfterStop = false;
    }

    wasPlayingLastLoop = shouldPlay;

    if (M5.BtnA.wasPressed()) {
        volume = max(0, volume - 2);
        savedVolume = (volume > 0) ? volume : savedVolume;
        M5.Speaker.setVolume(volume);
        printf_log("Volume: %d\n", volume);
    }

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

    if (M5.BtnC.wasPressed()) {
        volume = min(255, volume + 2);
        savedVolume = volume;
        M5.Speaker.setVolume(volume);
        printf_log("Volume: %d\n", volume);
    }
}

void showCurrentTimeOnce() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        char timeStr[32];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M", &timeinfo);
        printf_log("Time: %s (%s)\n", timeStr, timeinfo.tm_isdst ? "BST" : "GMT");
    } else {
        println_log("Time unavailable.");
    }
}

bool openWav(const char* path) {
    wavFile = SD.open(path);
    if (!wavFile) return false;

    char riff[4];
    wavFile.readBytes(riff, 4);
    if (strncmp(riff, "RIFF", 4) != 0) return false;

    wavFile.seek(20);
    wavFile.read(); wavFile.read();
    numChannels = wavFile.read() | (wavFile.read() << 8);
    sampleRate  = wavFile.read() | (wavFile.read() << 8) | (wavFile.read() << 16) | (wavFile.read() << 24);

    wavFile.seek(34);
    bitsPerSample = wavFile.read() | (wavFile.read() << 8);
    wavFile.seek(44);
    printf_log("WAV: %u ch, %u bits, %lu Hz\n", numChannels, bitsPerSample, sampleRate);
    return true;
}

bool readWiFiCredentials() {
    File file = SD.open("/ssid.txt");
    if (!file) return false;

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

bool loadSchedule(const char* path) {
    schedule.clear();
    File file = SD.open(path);
    if (!file) return false;

    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;

        int sep = line.indexOf('-');
        int space = line.indexOf(' ', sep);
        if (sep == -1 || space == -1) continue;

        String startStr = line.substring(0, sep);
        String endStr   = line.substring(sep + 1, space);
        String volStr   = line.substring(space + 1);
        int sh, sm, eh, em, vol;

        if (sscanf(startStr.c_str(), "%d:%d", &sh, &sm) != 2) continue;
        if (sscanf(endStr.c_str(), "%d:%d", &eh, &em) != 2) continue;
        if (sscanf(volStr.c_str(), "%d", &vol) != 1 || vol < 0 || vol > 255) continue;

        int startMin = sh * 60 + sm;
        int endMin   = eh * 60 + em;
        if (startMin >= 0 && startMin < 1440 && endMin > startMin && endMin <= 1440) {
            schedule.push_back({startMin, endMin, (uint8_t)vol});
        }
    }
    file.close();
    return !schedule.empty();
}

int getScheduledVolume() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return -1;

    int nowMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    for (const auto& range : schedule) {
        if (nowMin >= range.startMin && nowMin < range.endMin) {
            return range.volume;
        }
    }
    return -1;
}

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
