/**
 * @file   PinkNoisePlayer.ino
 * @brief  Schedules and plays a WAV audio file on an M5CoreS3, 
 *         with time-synchronized volume control and user interaction.
 *
 * This code initializes the M5CoreS3 hardware (display canvas, speaker, buttons),
 * mounts an SD card over SPI, and reads Wi-Fi credentials from `/ssid.txt`.
 * Upon startup, it connects to the specified Wi-Fi network and synchronizes
 * the real-time clock via NTP (GMT/BST time zone). It then opens a long-form
 * pink noise WAV file from the SD card (`/PinkNoiseLong.wav`) and optionally
 * loads a schedule of time ranges and corresponding volume levels from
 * `/schedule.txt`. **On startup, the player is muted** until either a schedule
 * slot becomes active or the user adjusts the volume via buttons.
 *
 * In the main loop, the code:
 *  1. Updates button states.
 *  2. Queries the current time to determine if a scheduled volume override applies.
 *  3. Sets the speaker volume based on either the schedule or a user-adjustable setting.
 *  4. Streams raw audio data from the WAV file to the speaker buffer, rewinding
 *     automatically when the end is reached.
 *  5. Logs volume changes, schedule starts/stops, and timestamps to both Serial
 *     and the on-screen canvas.
 *  6. Displays the next scheduled volume change time whenever the schedule takes effect.
 *  7. Monitors button A/B/C to decrease volume, toggle mute/unmute, and increase volume.
 *  8. If playback is paused or volume drops to zero, it re-syncs time once after the stop.
 *
 * Key Features:
 *  - **Silent on Startup**: `volume` is initialized to 0; `savedVolume` holds the restore level.
 *  - **SD Card Audio**: Reads WAV header to configure channels, bit depth, and sample rate.
 *  - **NTP Time Sync**: Uses `configTzTime` with DST rules for Europe.
 *  - **Scheduled Volume**: Parses human-readable schedule entries (HH:MM-HH:MM VOLUME).
 *  - **Dynamic Control**: Real-time adjustment via physical buttons; logs all changes.
 *  - **Next Change Notice**: Calculates and logs the next schedule boundary time.
 *  - **Resilience**: Halts on critical failures (SD init, file reads, Wi-Fi, WAV parsing).
 *
 * Dependencies:
 *   - M5CoreS3 library for display, speaker, and buttons
 *   - SPI and SD for file I/O
 *   - WiFi.h and time.h for network and NTP
 *   - Vector for schedule storage
 *
 * Usage:
 *   - Place `PinkNoiseLong.wav` and `schedule.txt` on the SD card root.
 *   - Provide SSID and password in `/ssid.txt` (two lines: SSID, then password).
 *   - Power on the M5CoreS3 and observe logging on the screen.
 */

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
int volume = 0;             // start muted
int savedVolume = 16;       // default “restore” volume
uint32_t sampleRate = 44100;
uint16_t bitsPerSample = 16;
uint8_t numChannels = 1;
bool speakerActive = true;
bool wasPlayingLastLoop = false;

int lastLoggedScheduledVol = -2; // track schedule-based volume

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
void logNextScheduledChange();

void setup() {
    M5.begin();
    M5.Speaker.begin();
    M5.Speaker.setVolume(volume);    // still muted

    // prepare display
    canvas.setColorDepth(1);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    canvas.setPaletteColor(1, RED);
    canvas.setTextSize(1.0);
    canvas.setTextScroll(true);
    canvas.fillSprite(BLACK);
    canvas.pushSprite(0, 0);

    // mount SD
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
    if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
        println_log("SD init failed.");
        while (1);
    }
    println_log("SD mounted.");

    // load Wi-Fi creds
    if (!readWiFiCredentials()) {
        println_log("Read /ssid.txt failed.");
        while (1);
    }

    // connect Wi-Fi
    printf_log("Connecting to WiFi (%s)...\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), password.c_str());
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        print_log(".");
    }
    println_log("\nWiFi Connected!");
    printf_log("IP: %s\n", WiFi.localIP().toString().c_str());

    // sync time
    configTzTime(tz, "pool.ntp.org");
    println_log("Syncing time...");
    struct tm timeinfo;
    while (!getLocalTime(&timeinfo)) {
        delay(500);
        print_log("*");
    }
    println_log("\nTime synced.");
    showCurrentTimeOnce();

    // open audio
    if (!openWav(WAV_PATH)) {
        println_log("Failed to open WAV.");
        while (1);
    }

    // load schedule (overrides mute when in-range)
    if (!loadSchedule(SCHEDULE_PATH)) {
        println_log("No valid schedule. Playing always (still muted until you change).");
    } else {
        println_log("Schedule loaded.");
        logNextScheduledChange();
    }

    println_log("Ready. (Muted on startup)");
}

void loop() {
    M5.update();
    static uint8_t buffer[512];

    int scheduledVol = getScheduledVolume();
    int effectiveVolume = (scheduledVol >= 0) ? scheduledVol : volume;
    bool shouldPlay = (effectiveVolume > 0 && isPlaying && speakerActive);

    // schedule start/stop logging
    if (scheduledVol != lastLoggedScheduledVol) {
        struct tm now; getLocalTime(&now);
        char ts[16];
        strftime(ts, sizeof(ts), "%H:%M", &now);
        if (scheduledVol >= 0) {
            printf_log("%s SCHEDULE START: vol=%d\n", ts, scheduledVol);
        } else {
            printf_log("%s SCHEDULE STOP\n", ts);
        }
        lastLoggedScheduledVol = scheduledVol;
        logNextScheduledChange();
    }

    if (shouldPlay) {
        M5.Speaker.setVolume(effectiveVolume);
        if (wavFile.available()) {
            size_t bytesRead = wavFile.read(buffer, sizeof(buffer));
            if (bytesRead > 0) {
                M5.Speaker.playRaw((int16_t*)buffer, bytesRead/2, sampleRate, numChannels==2, false);
            }
        } else {
            wavFile.seek(44);
        }
    } else {
        M5.Speaker.stop();
    }

    // Button A: lower manual volume
    if (M5.BtnA.wasPressed()) {
        volume = max(0, volume - 2);
        if (volume > 0) savedVolume = volume;
        M5.Speaker.setVolume(volume);
        printf_log("Manual Volume: %d\n", volume);
    }
    // Button B: toggle mute/unmute
    if (M5.BtnB.wasPressed()) {
        if (volume > 0) {
            savedVolume = volume;
            volume = 0;
            println_log("Manual PAUSE");
        } else {
            volume = savedVolume;
            println_log("Manual RESUME");
        }
        M5.Speaker.setVolume(volume);
    }
    // Button C: raise manual volume
    if (M5.BtnC.wasPressed()) {
        volume = min(255, volume + 2);
        savedVolume = volume;
        M5.Speaker.setVolume(volume);
        printf_log("Manual Volume: %d\n", volume);
    }
}

void logNextScheduledChange() {
    if (schedule.empty()) return;
    std::vector<int> events;
    for (auto &r : schedule) {
        events.push_back(r.startMin);
        events.push_back(r.endMin);
    }
    std::sort(events.begin(), events.end());
    events.erase(std::unique(events.begin(), events.end()), events.end());

    struct tm now; if (!getLocalTime(&now)) return;
    int nowMin = now.tm_hour*60 + now.tm_min;

    bool tomorrow = false;
    int nextMin = -1;
    for (int m : events) {
        if (m > nowMin) { nextMin = m; break; }
    }
    if (nextMin < 0) { nextMin = events[0]; tomorrow = true; }

    int hh = nextMin/60, mm = nextMin%60;
    char buf[40];
    if (tomorrow) {
        snprintf(buf, sizeof(buf), "Next scheduled change is tomorrow %02d:%02d", hh, mm);
    } else {
        snprintf(buf, sizeof(buf), "Next scheduled change is at %02d:%02d", hh, mm);
    }
    println_log(buf);
}

int getScheduledVolume() {
    struct tm ti; if (!getLocalTime(&ti)) return -1;
    int nowMin = ti.tm_hour*60 + ti.tm_min;
    for (auto &r : schedule) {
        if (nowMin >= r.startMin && nowMin < r.endMin) {
            return r.volume;
        }
    }
    return -1;
}

bool loadSchedule(const char* path) {
    schedule.clear();
    File file = SD.open(path);
    if (!file) return false;
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.startsWith("#") || line.isEmpty()) continue;
        int sep = line.indexOf('-'), sp = line.indexOf(' ', sep);
        if (sep<0||sp<0) continue;
        String s1 = line.substring(0,sep), s2=line.substring(sep+1,sp), sv=line.substring(sp+1);
        int sh,sm,eh,em,vol;
        if (sscanf(s1.c_str(),"%d:%d",&sh,&sm)!=2) continue;
        if (sscanf(s2.c_str(),"%d:%d",&eh,&em)!=2) continue;
        if (sscanf(sv.c_str(),"%d",&vol)!=1) continue;
        int st=sh*60+sm, en=eh*60+em;
        if (st<en&&st>=0&&en<=1440&&vol>=0&&vol<=255) {
            schedule.push_back({st,en,(uint8_t)vol});
        }
    }
    file.close();
    return !schedule.empty();
}

bool openWav(const char* path) {
    wavFile = SD.open(path);
    if (!wavFile) return false;
    char riff[4];
    wavFile.readBytes(riff,4);
    if (strncmp(riff,"RIFF",4)!=0) return false;
    wavFile.seek(20);
    wavFile.read(); wavFile.read();
    numChannels = wavFile.read() | (wavFile.read()<<8);
    sampleRate = wavFile.read() | (wavFile.read()<<8) |
                 (wavFile.read()<<16) | (wavFile.read()<<24);
    wavFile.seek(34);
    bitsPerSample = wavFile.read() | (wavFile.read()<<8);
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
    // trim then assign
    String t;
    t = lines[0]; t.trim();  ssid = t;
    t = lines[1]; t.trim();  password = t;
    return (ssid.length() > 0 && password.length() > 0);
}

void showCurrentTimeOnce() {
    struct tm ti;
    if (getLocalTime(&ti)) {
        char buf[64];
        strftime(buf, sizeof(buf), "Time: %Y-%m-%d %H:%M (%Z)", &ti);
        println_log(buf);
    } else {
        println_log("Time unavailable.");
    }
}

void println_log(const char *str) {
    Serial.println(str);
    canvas.println(str);
    canvas.pushSprite(0,0);
}
void print_log(const char *str) {
    Serial.print(str);
    canvas.print(str);
    canvas.pushSprite(0,0);
}
void printf_log(const char *format, ...) {
    char buf[256];
    va_list ap; va_start(ap,format);
    vsnprintf(buf,sizeof(buf),format,ap);
    va_end(ap);
    Serial.print(buf);
    canvas.printf(buf);
    canvas.pushSprite(0,0);
}
