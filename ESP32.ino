#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>
// #include <LiquidCrystal_I2C.h>  <-- LCD kütüphanesini kaldırdık
#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h> 

// WiFi ayarları
const char* ssid = "WIFI_ADI";
const char* password = "WIFI_SIFRESI";

// MQTT broker ayarları
const char* mqtt_server = "MQTT_SUNUCU_IP";
const int mqtt_port = 1883;
const char* mqtt_user = "";
const char* mqtt_password = "";
const char* client_id = "ESP32_Access_Control";

// MQTT konuları
const char* topic_motion = "access/motion";
const char* topic_card = "access/card";
const char* topic_password = "access/password";
const char* topic_system = "access/system";

const char* topic_card_result = "result/card";
const char* topic_password_result = "result/password";

// === PIN TANIMLARI ===
#define PIR_PIN    34
#define SS_PIN     5 
#define RST_PIN    4 
#define SERVO_PIN  13
#define LOCK_ANGLE 0 
#define OPEN_ANGLE 90 

Servo doorServo;

// === KEYPAD AYARLARI ===
const byte ROWS = 4; 
const byte COLS = 4; 
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {26, 25, 32, 33};
byte colPins[COLS] = {14, 27, 16, 17};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// LCD Nesnesi silindi --> LiquidCrystal_I2C lcd(0x27, 16, 2);

MFRC522 rfid(SS_PIN, RST_PIN);
WiFiClient espClient;
PubSubClient mqtt(espClient);

enum Modes { CARD, PASSWORD, CARD_PASSWORD };
Modes currentMode = CARD_PASSWORD;

String enteredPassword = "";
String scannedCardUID = "";
bool cardResult = false;
bool passwordResult = false;
bool resultReceived = false;

unsigned long lastReconnectAttempt = 0;
unsigned long operationStartTime = 0;
bool systemReady = false;

const int DOOR_OPEN_DURATION = 5000;
unsigned long doorOpenTime = 0;
bool doorOpen = false;
const unsigned long RECONNECT_INTERVAL = 10000;

#define ERROR_NONE 0
#define ERROR_TIMEOUT 1
#define ERROR_INVALID_CARD 2
#define ERROR_WRONG_PASSWORD 3
#define ERROR_CONNECTION_LOST 5

// Prototipler
void initializeSystem();
bool waitForInternetConnection(const unsigned long timeout = 30000);
bool connectToMQTT(const unsigned long timeout = 15000);
void displayMessage(String line1, String line2 = ""); // Artık Serial'a yazacak
void publishMessage(const char* topic, const char* message);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void handleSystemError(int errorCode);
bool authenticateCard(unsigned long timeout = 10000);
bool authenticatePassword(unsigned long timeout = 15000);
void executeAuthentication();
void openDoor();
void checkDoorStatus();

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  
  // LCD başlatma kodları silindi
  
  displayMessage("Sistem Baslatiliyor...");
  
  pinMode(PIR_PIN, INPUT_PULLDOWN); 
  
  doorServo.setPeriodHertz(50);    
  doorServo.attach(SERVO_PIN, 500, 2400); 
  doorServo.write(LOCK_ANGLE);     
  
  keypad.setDebounceTime(50);
  keypad.setHoldTime(1000); 
  
  SPI.begin();
  rfid.PCD_Init();
  
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  
  initializeSystem();
}

// ===== LOOP =====
void loop() {
  if (!mqtt.connected()) {
    systemReady = false;               
    unsigned long now = millis();
    if (now - lastReconnectAttempt > RECONNECT_INTERVAL) {
      lastReconnectAttempt = now;
      if (WiFi.status() != WL_CONNECTED) {
        if (!waitForInternetConnection()) return;
      }
      if (connectToMQTT()) {
        systemReady = true;
        displayMessage("Sistem Tekrar Hazir");
      }
    }
  } else {
    mqtt.loop();
  }
  
  if (!systemReady) return;
  
  checkDoorStatus();
  
  if (!doorOpen && digitalRead(PIR_PIN) == HIGH) {
    delay(50);
    if(digitalRead(PIR_PIN) == HIGH) { 
        displayMessage("Hareket Algilandi!");
        publishMessage(topic_motion, "Hareket algilandi");
        delay(1500);
        executeAuthentication();
    }
  }
}

// ===== AUTHENTICATION =====
void executeAuthentication() {
  bool authSuccess = true;
  
  switch (currentMode) {
    case CARD: displayMessage("MOD: Sadece Kart"); break;
    case PASSWORD: displayMessage("MOD: Sadece Sifre"); break;
    case CARD_PASSWORD: displayMessage("MOD: Kart + Sifre"); break;
  }
  delay(1000);
  
  if (currentMode == CARD || currentMode == CARD_PASSWORD) {
    authSuccess = authSuccess && authenticateCard();
    if (!authSuccess) return;
  }
  
  if (currentMode == PASSWORD || currentMode == CARD_PASSWORD) {
    authSuccess = authSuccess && authenticatePassword();
    if (!authSuccess) return;
  }
  
  if (authSuccess) {
    displayMessage("ERISIM IZNI VERILDI!");
    publishMessage(topic_system, "Basarili giris");
    delay(1500);
    openDoor();
    delay(1500);
  }
}

bool authenticateCard(unsigned long timeout) {
  displayMessage("Lutfen Karti Okutun...");
  operationStartTime = millis();
  scannedCardUID = "";
  cardResult = false;
  resultReceived = false;
  
  while (millis() - operationStartTime < timeout) {
    if (!mqtt.connected()) {
      handleSystemError(ERROR_CONNECTION_LOST);
      return false;
    }
    mqtt.loop();
    
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String uid = "";
      for (byte i = 0; i < rfid.uid.size; i++) {
        uid += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
        uid += String(rfid.uid.uidByte[i], HEX);
      }
      uid.toUpperCase();
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
      
      scannedCardUID = uid;
      displayMessage("Kart Okundu: " + scannedCardUID);
      publishMessage(topic_card, scannedCardUID.c_str());
      displayMessage("Sunucu Dogrulamasi Bekleniyor...");
      
      unsigned long responseWaitStart = millis();
      while (!resultReceived && (millis() - responseWaitStart < 5000)) {
        mqtt.loop();
        delay(10);
      }
      
      if (resultReceived) {
        if (cardResult) {
          displayMessage("KART ONAYLANDI");
          delay(1000);
          return true;
        } else {
          handleSystemError(ERROR_INVALID_CARD);
          return false;
        }
      } else {
        handleSystemError(ERROR_TIMEOUT);
        return false;
      }
    }
    delay(50);
  }
  handleSystemError(ERROR_TIMEOUT);
  return false;
}

// ===== ŞİFRE FONKSİYONU =====
bool authenticatePassword(unsigned long timeout) {
  displayMessage("Lutfen Sifreyi Girin:");
  
  operationStartTime = millis();
  enteredPassword = ""; 
  passwordResult = false;
  resultReceived = false;
  unsigned long responseWaitStart = 0;
  
  while (millis() - operationStartTime < timeout) {
    if (!mqtt.connected()) {
      handleSystemError(ERROR_CONNECTION_LOST);
      return false;
    }
    mqtt.loop();
    
    char key = keypad.getKey();
    if (key && key != NO_KEY) {
      operationStartTime = millis(); 
      // Serial.print("Basilan Tus: "); Serial.println(key); // Debug için açabilirsin
      
      switch (key) {
        case '*': 
          enteredPassword = "";
          Serial.println("\n[SISTEM]: Sifre temizlendi, bastan giriniz.");
          break;
          
        case '#': 
          if (enteredPassword.length() == 0) {
            displayMessage("UYARI: Sifre Bos Olamaz!");
            delay(1000);
            displayMessage("Sifreyi Girin:");
            break;
          }

          Serial.println("\n[GIRIS]: Sifre gonderiliyor...");
          // Güvenlik için şifreyi açık yazdırmıyoruz, sadece yıldızlı halini görelim
          // Serial.println(enteredPassword); 
          
          publishMessage(topic_password, enteredPassword.c_str());
          displayMessage("Sifre Dogrulaniyor...");
          
          resultReceived = false;
          responseWaitStart = millis();
          while (!resultReceived && (millis() - responseWaitStart < 5000)) {
            mqtt.loop();
            delay(10);
          }
          
          enteredPassword = ""; 
          
          if (resultReceived) {
            if (passwordResult) {
              displayMessage("SIFRE DOGRU!");
              delay(1500);
              return true;
            } else {
              handleSystemError(ERROR_WRONG_PASSWORD);
              return false;
            }
          } else {
            handleSystemError(ERROR_TIMEOUT);
            return false;
          }
          break;
          
        default: 
          if (enteredPassword.length() < 16) {
            enteredPassword += key;
            Serial.print("*"); // Her tuşa basıldığında yan yana yıldız koyar
          }
          break;
      }
    }
    delay(10);
  }
  
  handleSystemError(ERROR_TIMEOUT);
  return false;
}

void openDoor() {
  doorOpen = true;
  doorOpenTime = millis();
  doorServo.write(OPEN_ANGLE);
  displayMessage("KAPI ACILDI (" + String(DOOR_OPEN_DURATION/1000) + " sn)");
  publishMessage(topic_system, "Kapi acildi");
}

void checkDoorStatus() {
  if (doorOpen && (millis() - doorOpenTime >= DOOR_OPEN_DURATION)) {
    doorOpen = false;
    doorServo.write(LOCK_ANGLE);
    displayMessage("KAPI KAPANDI");
    publishMessage(topic_system, "Kapi kapandi");
    delay(1500);
    displayMessage("Sistem Hazir - Bekleniyor...");
  }
}

void initializeSystem() {
  systemReady = false;
  if (!waitForInternetConnection()) return;
  if (!connectToMQTT()) return;
  systemReady = true;
  displayMessage("Sistem Tamamen Hazir");
  publishMessage(topic_system, "Sistem hazir");
}

bool waitForInternetConnection(unsigned long timeout) {
  displayMessage("WiFi Baglaniyor...");
  WiFi.begin(ssid, password);
  unsigned long startTime = millis();
  
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startTime > timeout) return false;
    delay(500);
    Serial.print("."); // Yan yana nokta koyar
  }
  Serial.println(""); // Alt satıra geç
  displayMessage("WiFi Baglandi! IP: " + WiFi.localIP().toString());
  delay(2000);
  return true;
}

bool connectToMQTT(unsigned long timeout) {
  displayMessage("MQTT Sunucusuna Baglaniyor...");
  unsigned long startTime = millis();
  while (!mqtt.connected()) {
    if (millis() - startTime > timeout) return false;
    Serial.print(".");
    if (mqtt.connect(client_id, mqtt_user, mqtt_password)) {
      Serial.println("\n[MQTT]: Baglandi!");
      mqtt.subscribe(topic_card_result);
      mqtt.subscribe(topic_password_result);
      return true;
    } else {
      Serial.print("Hata, kod: ");
      Serial.print(mqtt.state());
      Serial.println(" (2sn bekleme)");
      delay(2000);
    }
  }
  return mqtt.connected();
}

// ===== YENİ EKRAN FONKSİYONU =====
// LCD yerine Serial Monitor'e formatlı yazı yazar
void displayMessage(String line1, String line2) {
  Serial.println("--------------------------------");
  Serial.println("[DURUM]: " + line1);
  if (line2 != "") {
    Serial.println("[DETAY]: " + line2);
  }
  Serial.println("--------------------------------");
}

void publishMessage(const char* topic, const char* message) {
  if (mqtt.connected()) {
    mqtt.publish(topic, message);
    Serial.println("[MQTT OUT] Konu: " + String(topic) + " | Mesaj: " + String(message));
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  
  Serial.println("[MQTT IN] Konu: " + topicStr + " | Mesaj: " + message);

  if (topicStr == topic_card_result) {
    resultReceived = true;
    cardResult = (message == "true" || message == "1");
  }
  else if (topicStr == topic_password_result) {
    resultReceived = true;
    passwordResult = (message == "true" || message == "1");
  }
}

void handleSystemError(int errorCode) {
  String errorMessage = "";
  switch (errorCode) {
    case ERROR_TIMEOUT: errorMessage = "Sure Asimi!"; break;
    case ERROR_INVALID_CARD: errorMessage = "Gecersiz Kart!"; break;
    case ERROR_WRONG_PASSWORD: errorMessage = "Hatali Sifre!"; break;
    case ERROR_CONNECTION_LOST: errorMessage = "Baglanti Kaybi!"; systemReady = false; break;
    default: errorMessage = "Bilinmeyen Hata!"; break;
  }
  displayMessage("HATA OLUSTU!", errorMessage);
  publishMessage(topic_system, ("Hata: " + errorMessage).c_str());
  delay(3000);
  if (systemReady) displayMessage("Sistem Hazir - Bekleniyor...");
}