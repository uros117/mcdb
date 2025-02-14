#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>

// Security callback class
class BLESecurityCallbacksa : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() { return 000000; }
    void onPassKeyNotify(uint32_t pass_key) {}
    bool onConfirmPIN(uint32_t pass_key) { return true; }
    bool onSecurityRequest() { return true; }
    void onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl) {
        if(auth_cmpl.success) {
            Serial.println("BLE Authentication Success");
        } else {
            Serial.println("BLE Authentication Failure");
        }
    }
};

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

// Initialize display
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Nordic UART Service UUIDs
#define SERVICE_UUID           "290a0ad4-7d8d-4842-9901-132af70f71b9"
#define CHARACTERISTIC_UUID_RX "5f472cc0-8c38-4320-9fb0-f646e77faff6"
#define CHARACTERISTIC_UUID_TX "17fcb492-3e32-456e-af32-67322a771f48"

BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;

void updateDisplay(String message);

class DisplayServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      Serial.println("BT connected");
      deviceConnected = true;
      updateDisplay("Connected!");
    };

    void onDisconnect(BLEServer* pServer) {
      Serial.println("BT disconnected");
      deviceConnected = false;
      updateDisplay("Disconnected");
      // Restart advertising
      pServer->startAdvertising();
    }
};

class DisplayReceiveCallback: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      std::string rxValue = pCharacteristic->getValue();
      char *cstr = new char[rxValue.length() + 1];
      strcpy(cstr, rxValue.c_str());

      Serial.println("Received data:");
      Serial.println(cstr);
      updateDisplay(String(cstr));
      
      delete[] cstr;  // Clean up allocated memory
    }
};

void updateDisplay(String message) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  
  // Word wrap implementation
  int16_t x1, y1;
  uint16_t w, h;
  String currentLine = "";
  String words = message;
  int currentY = 0;
  int lineHeight = 8; // Height of text with size 1
  
  while (words.length() > 0 && currentY < SCREEN_HEIGHT) {
    int spaceIndex = words.indexOf(' ');
    String word = (spaceIndex == -1) ? words : words.substring(0, spaceIndex);
    
    display.getTextBounds(currentLine + word, 0, 0, &x1, &y1, &w, &h);
    
    if (w > SCREEN_WIDTH) {
      if (currentLine.length() > 0) {
        display.setCursor(0, currentY);
        display.println(currentLine);
        currentY += lineHeight;
        currentLine = word + " ";
      } else {
        // Word is too long, need to split it
        display.setCursor(0, currentY);
        display.println(word);
        currentY += lineHeight;
        currentLine = "";
      }
    } else {
      currentLine += word + " ";
    }
    
    if (spaceIndex == -1) {
      words = "";
    } else {
      words = words.substring(spaceIndex + 1);
    }
  }
  
  if (currentLine.length() > 0 && currentY < SCREEN_HEIGHT) {
    display.setCursor(0, currentY);
    display.println(currentLine);
  }
  
  display.display();
}

void initBLE() {
  Serial.println("Initializing BLE...");
  BLEDevice::init("ESP32-C3 Display");
  
  // Set the security settings
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  BLEDevice::setSecurityCallbacks(new BLESecurityCallbacksa());
  
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new DisplayServerCallbacks());
  
  // Set security
  BLESecurity *pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
  pSecurity->setCapability(ESP_IO_CAP_NONE);
  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  // Create TX characteristic
  pTxCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID_TX,
                      BLECharacteristic::PROPERTY_NOTIFY |
                      BLECharacteristic::PROPERTY_READ
                    );
  pTxCharacteristic->addDescriptor(new BLE2902());
  
  // Create RX characteristic
  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
                                         CHARACTERISTIC_UUID_RX,
                                         BLECharacteristic::PROPERTY_WRITE
                                       );
  pRxCharacteristic->setCallbacks(new DisplayReceiveCallback());

  pService->start();
  
  // Configure advertising
  BLEAdvertising *pAdvertising = pServer->getAdvertising();
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // functions that help with iPhone connections issue
  pAdvertising->setMinPreferred(0x12);
  pAdvertising->start();
  Serial.println("BLE device ready");
}

void setup() {
  Serial.begin(115200);

  // Initialize OLED display
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  }
  display.clearDisplay();
  updateDisplay("Starting...");

  // Initialize BLE
  initBLE();
}

void loop() {
  // Manage BLE connection status changes
  if (!deviceConnected && oldDeviceConnected) {
    delay(500); // Give the bluetooth stack the chance to get things ready
    pServer->startAdvertising(); // Restart advertising
    oldDeviceConnected = deviceConnected;
  }
  
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }
  
  delay(100); // Add a small delay to prevent watchdog issues
}

// Optional: Function to send data back to the connected device
void writeValue(const char* value) {
    if (deviceConnected) {
        pTxCharacteristic->setValue(value);
        pTxCharacteristic->notify();
    }
}