#include <M5Stack.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEClient.h>
#include <BLE2902.h>

// Standard Heart Rate Service and Measurement characteristic UUIDs:
#define HR_SERVICE_UUID        "0000180d-0000-1000-8000-00805f9b34fb"
#define HR_MEASUREMENT_UUID    "00002a37-0000-1000-8000-00805f9b34fb"

// BLE objects
BLEScan*           pBLEScan     = nullptr;
BLEClient*         pClient      = nullptr;
BLERemoteCharacteristic* pHRChar = nullptr;
bool               doConnect    = false;
BLEAdvertisedDevice* myDevice   = nullptr;

// Called for each advertisement received during scanning
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (advertisedDevice.haveServiceUUID() &&
        advertisedDevice.isAdvertisingService(BLEUUID(HR_SERVICE_UUID))) {
      // <-- fixed here: split into print + println(c_str())
      M5.Lcd.print ("Found HRM: ");
      M5.Lcd.println(advertisedDevice.getName().c_str());

      pBLEScan->stop();
      myDevice  = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
    }
  }
};

// Called when the client connects/disconnects
class MyClientCallbacks : public BLEClientCallbacks {
  void onConnect(BLEClient* c) override {
    M5.Lcd.println("BLE Central: connected");
  }
  void onDisconnect(BLEClient* c) override {
    M5.Lcd.println("BLE Central: disconnected");
    // restart scan
    doConnect = false;
    delay(100);
    pBLEScan->start(0, nullptr, false);
  }
};

// This fires on each HR notification
static void hrNotifyCallback(
  BLERemoteCharacteristic* ,
  uint8_t*                 pData,
  size_t                   length,
  bool                     ) 
{
  if (length < 2) return;
  uint8_t hr = pData[1];
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.println("Heart Rate Monitor");
  M5.Lcd.printf("BPM: %u\n", hr);
}

void setup() {
  M5.begin();
  M5.Lcd.setTextSize(2);
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.println("BLE HRM Central");
  M5.Speaker.mute();

  // Init BLE in Central role
  BLEDevice::init("M5Stack-Central");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  M5.Lcd.println("Scanning for HRM...");
  pBLEScan->start(0, nullptr, false);
}

void loop() {
  // If a device was found, connect
  if (doConnect && myDevice) {
    M5.Lcd.println("Connecting...");
    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(new MyClientCallbacks());
    if (pClient->connect(myDevice)) {
      M5.Lcd.println("Connected. Discovering service...");
      BLERemoteService* svc = pClient->getService(HR_SERVICE_UUID);
      if (svc) {
        pHRChar = svc->getCharacteristic(HR_MEASUREMENT_UUID);
        if (pHRChar) {
          pHRChar->registerForNotify(hrNotifyCallback);
          // enable notifications
          pHRChar->getDescriptor(BLEUUID((uint16_t)0x2902))
                ->writeValue((uint8_t*)"\x01\x00", 2, true);
          M5.Lcd.println("Notifications ON");
        } else {
          M5.Lcd.println("No HR char.");
        }
      } else {
        M5.Lcd.println("No HR service.");
      }
    } else {
      M5.Lcd.println("Conn failed.");
    }
    doConnect = false;
  }

  // Button A to rescan
  if (M5.BtnA.wasPressed()) {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.println("Rescan HRM...");
    if (pClient && pClient->isConnected()) {
      pClient->disconnect();
    }
    delay(100);
    pBLEScan->start(0, nullptr, false);
  }

  M5.update();
  delay(20);
}
