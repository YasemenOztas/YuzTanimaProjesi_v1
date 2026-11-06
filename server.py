import paho.mqtt.client as mqtt
import time
from datetime import datetime
import os
import csv


# MQTT broker ayarları
MQTT_BROKER = "MQTT_SUNUCU_IP"
MQTT_PORT = 1883
MQTT_USER = ""
MQTT_PASS = ""

# MQTT konuları
TOPIC_CARD = "access/card"
TOPIC_PASSWORD = "access/password"
TOPIC_MOTION = "access/motion"
TOPIC_SYSTEM = "access/system"

# Yanıt gönderilecek topicler
TOPIC_CARD_RESULT = "result/card"
TOPIC_PASSWORD_RESULT = "result/password"

# Klasörler
LOG_DIR = "./logs"
if not os.path.exists(LOG_DIR):
    os.makedirs(LOG_DIR)

# Log dosyası ayarları
LOG_FILE = os.path.join(LOG_DIR, f"access_log_{datetime.now().strftime('%Y%m%d')}.csv")

# VERİTABANI
USERS_DB = {
    "users": [
        {
            "id": "user1",
            "name": "Kullanici Adi",
            "card_id": "KART_UID",
            "password": "SIFRE"
        },
        {
            "id": "user2",
            "name": "Kullanici Adi",
            "card_id": "KART_UID",
            "password": "SIFRE"
        }
    ]
}

# Doğrulama sürecindeki aktif kullanıcıyı tutar
current_user = None


# FONKSİYONLAR

def log_event(event_type, details):
    """Erişim olaylarını CSV formatında kaydeder"""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    file_exists = os.path.isfile(LOG_FILE)
    
    try:
        with open(LOG_FILE, 'a', newline='', encoding='utf-8') as file:
            writer = csv.writer(file)
            if not file_exists:
                writer.writerow(['Tarih', 'Olay Türü', 'Detaylar'])
            writer.writerow([timestamp, event_type, details])
        print(f"Log: [{event_type}] {details}")
    except Exception as e:
        print(f"Log yazma hatası: {e}")


# MQTT FONKSİYONLARI

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("MQTT Broker'a Bağlanıldı!")
        client.subscribe([
            (TOPIC_CARD, 0), 
            (TOPIC_PASSWORD, 0), 
            (TOPIC_MOTION, 0),
            (TOPIC_SYSTEM, 0)
        ])
        log_event("Sistem", "Sunucu başlatıldı ve MQTT'ye bağlandı")
    else:
        print(f"Bağlantı Başarısız, kod: {rc}")

def on_message(client, userdata, msg):
    global current_user
    topic = msg.topic
    payload = msg.payload.decode().strip() # Boşlukları temizler
    
    # 1. HAREKET ALGILAMA
    if topic == TOPIC_MOTION:
        log_event("Hareket", payload)

    # 2. SİSTEM MESAJLARI
    elif topic == TOPIC_SYSTEM:
        log_event("Sistem Raporu", payload)
        # Sistem yeniden başladığında veya hazır olduğunda kullanıcı oturumunu sıfırlar
        if "hazir" in payload.lower() or "baglandi" in payload.lower():
            current_user = None

    # 3. KART DOĞRULAMA
    elif topic == TOPIC_CARD:
        card_id = payload
        print(f"\n--- Kart Okundu: {card_id} ---")
        
        found_user = None
        for u in USERS_DB["users"]:
            if u["card_id"].upper() == card_id.upper():
                found_user = u
                break
        
        if found_user:
            current_user = found_user # Kullanıcıyı hafızaya al
            client.publish(TOPIC_CARD_RESULT, "true")
            log_event("Kart Doğrulama", f"BAŞARILI: {found_user['name']} (ID: {card_id})")
        else:
            current_user = None
            client.publish(TOPIC_CARD_RESULT, "false")
            log_event("Kart Doğrulama", f"BAŞARISIZ: Tanımsız Kart (ID: {card_id})")

    # 4. ŞİFRE DOĞRULAMA
    elif topic == TOPIC_PASSWORD:
        password = payload
        print(f"\n--- Şifre Girildi: {password} ---")
        
        # Önce kart okutulmuşsa (current_user hafızada var)
        if current_user is not None:
            if current_user["password"] == password:
                client.publish(TOPIC_PASSWORD_RESULT, "true")
                log_event("Şifre Doğrulama", f"BAŞARILI: {current_user['name']} şifresi doğrulandı.")
                # Kullanıcıyı sıfırla (Kapı açılacak)
                current_user = None 
            else:
                client.publish(TOPIC_PASSWORD_RESULT, "false")
                log_event("Şifre Doğrulama", f"BAŞARISIZ: {current_user['name']} için yanlış şifre.")
                current_user = None # Oturumu düşür
        
        # Sadece şifre ile giriş (current_user yok)
        else:
            found_by_pass = None
            for u in USERS_DB["users"]:
                if u["password"] == password:
                    found_by_pass = u
                    break
            
            if found_by_pass:
                client.publish(TOPIC_PASSWORD_RESULT, "true")
                log_event("Şifre Doğrulama", f"BAŞARILI: {found_by_pass['name']} (Sadece Şifre)")
            else:
                client.publish(TOPIC_PASSWORD_RESULT, "false")
                log_event("Şifre Doğrulama", f"BAŞARISIZ: Tanımsız Şifre")


# ANA PROGRAM
def main():
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    
    # MQTT kullanıcı adı/şifre (yok)
    if MQTT_USER and MQTT_PASS:
        client.username_pw_set(MQTT_USER, MQTT_PASS)
    
    try:
        print("Sunucu Başlatılıyor...")
        client.connect(MQTT_BROKER, MQTT_PORT, 60)
        client.loop_forever()
        
    except KeyboardInterrupt:
        print("\nProgram kapatılıyor...")
    except Exception as e:
        print(f"Hata: {e}")
    finally:
        client.disconnect()

if __name__ == "__main__":
    main()