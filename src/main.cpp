#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLESecurity.h>

// Store last message in RTC memory
RTC_DATA_ATTR char lastMessage[128] = "No messages yet";

// Pin definitions
#define WAKEUP_PIN GPIO_NUM_2  // GPIO2 for button interrupt
#define BUTTON_PIN 2           // Same as WAKEUP_PIN but for Arduino digitalWrite
const uint64_t SLEEP_TIMEOUT = 60000;  // Sleep after 60 seconds of inactivity
const uint64_t LIGHT_SLEEP_TIMEOUT = 5000;  // Light sleep after 5 seconds when connected
unsigned long lastActivityTime = 0;

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
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;

void updateDisplay(String message);
void goToDeepSleep();
void goToLightSleep();
void updateLastActivity();

// Button interrupt handler
void IRAM_ATTR buttonISR() {
    // Reset activity timer on button press
    lastActivityTime = millis();
}

class DisplayServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      Serial.println("BT connected");
      deviceConnected = true;
      updateDisplay("Connected!");
      updateLastActivity();
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
      // Store message in RTC memory
      strncpy(lastMessage, cstr, sizeof(lastMessage) - 1);
      lastMessage[sizeof(lastMessage) - 1] = '\0';  // Ensure null termination
      
      updateDisplay(String(cstr));
      updateLastActivity();
      
      delete[] cstr;  // Clean up allocated memory
    }
};

void updateLastActivity() {
    lastActivityTime = millis();
}

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
    int lineHeight = 8;
    
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
    updateLastActivity();
}

void goToDeepSleep() {
    Serial.println("Going to deep sleep...");
    display.clearDisplay();
    display.display();
    
    // Turn off display
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    
    // Configure GPIO wake up source
    const uint64_t gpio_mask = (1ULL << BUTTON_PIN);
    esp_deep_sleep_enable_gpio_wakeup(gpio_mask, ESP_GPIO_WAKEUP_GPIO_LOW);
    
    // Go to deep sleep
    esp_deep_sleep_start();
}

void goToLightSleep() {
    Serial.println("Going to light sleep...");
    display.clearDisplay();
    display.display();
    
    // Turn off display
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    
    // Configure GPIO wake up source for light sleep
    gpio_wakeup_enable((gpio_num_t)BUTTON_PIN, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_wifi_wakeup();
    
    // Enable BLE wakeup
    esp_bt_sleep_enable();
    
    // Go to light sleep
    esp_light_sleep_start();
    
    // After waking up
    Serial.println("Woke from light sleep");
    
    // Turn display back on
    display.ssd1306_command(SSD1306_DISPLAYON);
    updateDisplay("Awake!");
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
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMinPreferred(0x12);
    pAdvertising->start();
    Serial.println("BLE device ready");
}

void setup() {
    Serial.begin(460800);

    // Configure wake up button
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);

    // Check wake up reason
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    
    // Initialize OLED display
    if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println(F("SSD1306 allocation failed"));
        for(;;);
    }
    display.clearDisplay();
    
    // Turn display on since we're waking up
    display.ssd1306_command(SSD1306_DISPLAYON);
    
    String wakeMessage;
    if(wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
        wakeMessage = "Button wake!\n";
    } else if(wakeup_reason == ESP_SLEEP_WAKEUP_BT) {
        wakeMessage = "BLE wake!\n";
    } else {
        wakeMessage = "Starting...\n";
    }
    
    // Add last message to wake message
    wakeMessage += "Last msg: ";
    wakeMessage += lastMessage;
    updateDisplay(wakeMessage);

    // Initialize BLE
    initBLE();
    
    // Initialize last activity time
    lastActivityTime = millis();
}

void loop() {
    if (deviceConnected) {
        // When connected, use light sleep after short timeout
        if (millis() - lastActivityTime > LIGHT_SLEEP_TIMEOUT) {
            goToLightSleep();
            updateLastActivity(); // Reset timer after wake
        }
    } else {
        // When disconnected, use deep sleep after longer timeout
        if (millis() - lastActivityTime > SLEEP_TIMEOUT) {
            goToDeepSleep();
        }
    }
    
    // Manage BLE connection status changes
    if (!deviceConnected && oldDeviceConnected) {
        delay(500);
        pServer->startAdvertising();
        oldDeviceConnected = deviceConnected;
    }
    
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
    }
    
    delay(100);
}

// Optional: Function to send data back to the connected device
void writeValue(const char* value) {
    if (deviceConnected) {
        pTxCharacteristic->setValue(value);
        pTxCharacteristic->notify();
    }
}