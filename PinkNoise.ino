// For this sketch: select Tools -> Partition Scheme -> Huge App (3MB)

#include <M5CoreS3.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <time.h>
#include <vector>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <BLE2902.h>

// SD and Audio settings
#define SD_SPI_SCK_PIN  18
#define SD_SPI_MISO_PIN 19
#define SD_SPI_MOSI_PIN 23
#define SD_SPI_CS_PIN   4
#define WAV_PATH "/PinkNoiseLong.wav"
#define SCHEDULE_PATH "/schedule.txt"
#define HR_LOG_PATH "/hr_log.csv"

// WiFi credentials
String ssid = "", password = "";

// Timezone (read from SD or default)
String tz;
const char* DEFAULT_TZ = "GMT0BST,M3.5.0/1,M10.5.0/2";

// BLE Heart Rate Service UUIDs
#define HR_SERVICE_UUID        "0000180d-0000-1000-8000-00805f9b34fb"
#define HR_MEASUREMENT_UUID    "00002a37-0000-1000-8000-00805f9b34fb"

// BLE objects
BLEScan* pBLEScan     = nullptr;
BLEClient* pClient    = nullptr;
BLERemoteCharacteristic* pHRChar = nullptr;
bool doConnect        = false;
BLEAdvertisedDevice* myDevice = nullptr;

// Audio & Playback
File wavFile;
bool isPlaying = true;
int volume = 0;
int savedVolume = 16;
uint32_t sampleRate = 44100;
uint16_t bitsPerSample = 16;
uint8_t numChannels = 1;
bool speakerActive = true;
bool wasPlayingLastLoop = false;

// Scheduling
typedef struct { int startMin, endMin; uint8_t vol; } TimeRange;
std::vector<TimeRange> schedule;
int lastLoggedScheduledVol = -2;

// Canvas for display
M5Canvas canvas(&M5.Display);

// Heart rate log file
File hrLogFile;

// Logging helpers
void println_log(const char *str) {
  Serial.println(str);
  canvas.println(str);
  canvas.pushSprite(0, 0);
}
void printf_log(const char *format, ...) {
  char buf[256];
  va_list ap; va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  Serial.print(buf);
  canvas.printf(buf);
  canvas.pushSprite(0, 0);
}

// Read WiFi creds
bool readWiFiCredentials() {
  File file = SD.open("/ssid.txt");
  if (!file) return false;
  String lines[2]; int idx=0;
  while (file.available() && idx<2) lines[idx++] = file.readStringUntil('\n');
  file.close();
  lines[0].trim(); lines[1].trim();
  ssid = lines[0]; password = lines[1];
  return ssid.length()>0 && password.length()>0;
}

// Read timezone from SD
void loadTimezone() {
  File tzFile = SD.open("/timezone.txt");
  if (tzFile) {
    tz = tzFile.readStringUntil('\n');
    tz.trim();
    tzFile.close();
    if (tz.length()==0) tz = DEFAULT_TZ;
  } else {
    tz = DEFAULT_TZ;
  }
  printf_log("Using timezone: %s\n", tz.c_str());
}

// Schedule parsing & volume
bool loadSchedule(const char* path) {
  schedule.clear();
  File file = SD.open(path);
  if (!file) return false;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim(); if (line.startsWith("#")||line.isEmpty()) continue;
    int dash = line.indexOf('-'), sp = line.indexOf(' ', dash);
    if (dash<0||sp<0) continue;
    String s1 = line.substring(0,dash);
    String s2 = line.substring(dash+1, sp);
    String sv = line.substring(sp+1);
    int sh,sm,eh,em,vol;
    if (sscanf(s1.c_str(), "%d:%d", &sh, &sm)!=2) continue;
    if (sscanf(s2.c_str(), "%d:%d", &eh, &em)!=2) continue;
    if (sscanf(sv.c_str(), "%d", &vol)!=1) continue;
    int st=sh*60+sm, en=eh*60+em;
    if (st<en && st>=0 && en<=1440 && vol>=0 && vol<=255) {
      schedule.push_back({st,en,(uint8_t)vol});
    }
  }
  file.close();
  return !schedule.empty();
}
int getScheduledVolume() {
  struct tm ti; if (!getLocalTime(&ti)) return -1;
  int nowMin = ti.tm_hour*60 + ti.tm_min;
  for (auto&r : schedule) if (nowMin>=r.startMin && nowMin<r.endMin) return r.vol;
  return -1;
}
void logNextScheduledChange() {
  if (schedule.empty()) return;
  std::vector<int> ev;
  for (auto&r : schedule) { ev.push_back(r.startMin); ev.push_back(r.endMin); }
  std::sort(ev.begin(), ev.end()); ev.erase(unique(ev.begin(),ev.end()), ev.end());
  struct tm now; if(!getLocalTime(&now)) return;
  int nowMin=now.tm_hour*60+now.tm_min;
  bool tomorrow=false; int next=-1;
  for(int m:ev) if(m>nowMin){ next=m; break; }
  if(next<0){ next=ev[0]; tomorrow=true; }
  int hh=next/60, mm=next%60;
  char buf[64];
  if(tomorrow) snprintf(buf,sizeof(buf),"Next scheduled change is tomorrow %02d:%02d", hh, mm);
  else snprintf(buf,sizeof(buf),"Next scheduled change is at %02d:%02d", hh, mm);
  println_log(buf);
}

// WAV open
bool openWav(const char* path) {
  wavFile = SD.open(path);
  if(!wavFile) return false;
  char riff[4]; wavFile.readBytes(riff,4);
  if(strncmp(riff,"RIFF",4)!=0) return false;
  wavFile.seek(20); wavFile.read(); wavFile.read();
  numChannels = wavFile.read()|(wavFile.read()<<8);
  sampleRate = wavFile.read()|(wavFile.read()<<8)|(wavFile.read()<<16)|(wavFile.read()<<24);
  wavFile.seek(34); bitsPerSample = wavFile.read()|(wavFile.read()<<8);
  wavFile.seek(44);
  printf_log("WAV: %u ch, %u bits, %lu Hz\n", numChannels, bitsPerSample, sampleRate);
  return true;
}

// BLE callbacks
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice adv) override {
    if(adv.haveServiceUUID() && adv.isAdvertisingService(BLEUUID(HR_SERVICE_UUID))) {
      println_log("Found HR Monitor");
      pBLEScan->stop(); myDevice=new BLEAdvertisedDevice(adv); doConnect=true;
    }
  }
};
class MyClientCallbacks: public BLEClientCallbacks {
  void onConnect(BLEClient* c) override { println_log("BLE Connected"); }
  void onDisconnect(BLEClient* c) override { println_log("BLE Disconnected"); doConnect=false; delay(100); pBLEScan->start(0,nullptr,false); }
};
static void hrNotifyCallback(BLERemoteCharacteristic*, uint8_t* pData, size_t length, bool) {
  if(length<2||!hrLogFile) return;
  uint8_t hr = pData[1];
  struct tm now; if(getLocalTime(&now)){
    char ts[32]; strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",&now);
    hrLogFile.printf("%s,%u\n", ts, hr);
    hrLogFile.flush();
    printf_log("%s | HR: %u BPM\n", ts, hr);
  }
}

void setup() {
  M5.begin(); Serial.begin(115200);
  // Setup display canvas
  canvas.setColorDepth(1);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  canvas.setPaletteColor(1, RED);
  canvas.setTextSize(1.5);  // 50% larger text
  canvas.setTextScroll(true);
  canvas.fillSprite(BLACK);
  canvas.pushSprite(0,0);

  // SD init
  SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
  if(!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) { println_log("SD init failed."); while(1); }
  println_log("SD mounted");

  // Open HR log file
  hrLogFile = SD.open(HR_LOG_PATH, FILE_APPEND);
  if(!hrLogFile) println_log("HR log open failed");

  // Load WiFi credentials
  if(!readWiFiCredentials()){ println_log("/ssid.txt read failed"); while(1); }

  // Connect to WiFi
  printf_log("Connecting to WiFi (%s)...\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), password.c_str());
  while(WiFi.status()!=WL_CONNECTED){ delay(500); canvas.print("."); canvas.pushSprite(0,0);}  println_log("\nWiFi Connected");

  // Load timezone and sync time
  loadTimezone();
  configTzTime(tz.c_str(), "pool.ntp.org");
  println_log("Syncing time...");
  while(!getLocalTime(new struct tm)){ delay(500); canvas.print("*"); canvas.pushSprite(0,0);} println_log("Time synced");

  // Pink noise setup
  if(!openWav(WAV_PATH)){ println_log("WAV open failed"); while(1);}  
  if(!loadSchedule(SCHEDULE_PATH)) println_log("No schedule loaded"); else { println_log("Schedule loaded"); logNextScheduledChange(); }

  // BLE init
  BLEDevice::init("M5CoreS3-HRCentral");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  println_log("Scanning for HR...");
  pBLEScan->start(0, nullptr, false);
}

void loop() {
  M5.update();
  // BLE connect
  if(doConnect && myDevice) {
    println_log("Connecting BLE...");
    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallbacks());
    if(pClient->connect(myDevice)){
      println_log("Connected to HR service");
      BLERemoteService* svc = pClient->getService(HR_SERVICE_UUID);
      if(svc){
        pHRChar = svc->getCharacteristic(HR_MEASUREMENT_UUID);
        if(pHRChar) {
          pHRChar->registerForNotify(hrNotifyCallback);
          pHRChar->getDescriptor(BLEUUID((uint16_t)0x2902))->writeValue((uint8_t*)"\x01\x00",2,true);
          println_log("HR notifications ON");
        }
      }
    } else println_log("BLE connect failed");
    doConnect = false;
  }

  // Pink noise playback & scheduling
  static uint8_t buf[512];
  int schedVol = getScheduledVolume();
  int effVol = (schedVol>=0)?schedVol:volume;
  bool shouldPlay = (effVol>0 && isPlaying && speakerActive);
  if(schedVol!=lastLoggedScheduledVol) {
    struct tm now; getLocalTime(&now);
    char ts[16]; strftime(ts,sizeof(ts),"%H:%M",&now);
    if(schedVol>=0) printf_log("%s SCHEDULE START: vol=%d\n", ts, schedVol);
    else println_log("SCHEDULE STOP");
    lastLoggedScheduledVol = schedVol;
    logNextScheduledChange();
  }
  if(shouldPlay) {
    M5.Speaker.setVolume(effVol);
    if(wavFile.available()){ size_t r = wavFile.read(buf, sizeof(buf)); if(r>0) M5.Speaker.playRaw((int16_t*)buf, r/2, sampleRate, numChannels==2, false); }
    else wavFile.seek(44);
  } else M5.Speaker.stop();

  // Buttons
  if(M5.BtnA.wasPressed()){ volume=max(0,volume-2); if(volume>0) savedVolume=volume; M5.Speaker.setVolume(volume); printf_log("Manual Vol: %d\n", volume);}  
  if(M5.BtnB.wasPressed()){ if(volume>0){ savedVolume=volume; volume=0; println_log("Manual PAUSE");} else{ volume=savedVolume; println_log("Manual RESUME");} M5.Speaker.setVolume(volume);}  
  if(M5.BtnC.wasPressed()){ volume=min(255,volume+2); savedVolume=volume; M5.Speaker.setVolume(volume); printf_log("Manual Vol: %d\n", volume);}  

  delay(20);
}
