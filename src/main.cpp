#include <Arduino.h>
#include <Wire.h>
#include <rgb_lcd.h>
#include <SensirionI2cScd30.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP_Mail_Client.h>
#include "secrets.h"

#define SCD30_I2C_ADDR_61 0x61
#define CO2_ALERT_THRESHOLD 1000
#define EMAIL_COOLDOWN 60000  // 1 minute

const char* mqttServer = "mqtt3.thingspeak.com";
const int mqttPort = 1883;
const char* topic = "channels/3141928/publish";

rgb_lcd lcd;
SensirionI2cScd30 scd30;
WiFiClient wifiClient;
PubSubClient client(wifiClient);
SMTPSession smtp;

unsigned long lastEmailSent = 0;
bool emailAlertSent = false;
const int buzzer = 14;

// Prototypes
void reconnectMQTT();
void connectWiFi();
bool readSensorWithRetry(float& co2, float& temperature, float& humidity, int maxRetries = 3);
bool publishWithRetry(const char* topic, const char* payload, int maxRetries = 3);
void sendEmailAlert(float co2, float temperature, float humidity);
void checkCO2Alert(float co2, float temperature, float humidity);
void smtpCallback(SMTP_Status status);

void setup() {
    Serial.begin(115200);
    pinMode(buzzer, OUTPUT);

    lcd.begin(16, 2);

    connectWiFi();

    Wire.begin();
    scd30.begin(Wire, SCD30_I2C_ADDR_61);

    client.setServer(mqttServer, mqttPort);

    smtp.debug(0);
    smtp.callback(smtpCallback);

    lcd.setCursor(0, 0);
    lcd.print("MQTT setup done");
    delay(2000);
    lcd.clear();
}

void loop() {

    if (!client.connected()) {
        reconnectMQTT();
    }

    client.loop();

    float co2, temperature, humidity;

    if (!readSensorWithRetry(co2, temperature, humidity)) {
        lcd.clear();
        lcd.print("Sensor Error!");
        delay(2000);
        return;
    }

    checkCO2Alert(co2, temperature, humidity);

    String payload = 
        "field1=" + String(co2) +
        "&field2=" + String(temperature) +
        "&field3=" + String(humidity) +
        "&status=MQTTPUBLISH";

    Serial.println("Publishing payload:");
    Serial.println(payload);
    delay(2000);

    lcd.clear();

    if (!publishWithRetry(topic, (char*)payload.c_str())) {
        lcd.print("Publish Failed!");
        delay(2000);
        lcd.clear();
    }

    lcd.setCursor(0, 0);
    lcd.print("CO2: ");
    lcd.print(int(co2));
    lcd.print(" ppm");

    lcd.setCursor(0, 1);
    lcd.print("T: ");
    lcd.print(int(temperature));
    lcd.write(0xDF);
    lcd.print("C");

    lcd.setCursor(8, 1);
    lcd.print("H: ");
    lcd.print(int(humidity));
    lcd.write(0x25);

    if (int(co2) > 1000) {
        digitalWrite(buzzer, HIGH);
        delay(1000);
    }

    digitalWrite(buzzer, LOW);
    delay(1000);
}

// ======================= FUNCTIONS =======================

void connectWiFi() {
    int attempts = 0;
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        lcd.setCursor(0, 0);
        lcd.print("Trying WIFI...");
        attempts++;
    }

    lcd.clear();

    if (WiFi.status() == WL_CONNECTED) {
        lcd.print("WiFi Connected!");
        delay(1500);
        lcd.clear();
        Serial.println("WiFi connected!");
    } else {
        lcd.print("WiFi Failed!");
        delay(2000);
        lcd.clear();
    }
}

void reconnectMQTT() {
    while (!client.connected()) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Connecting MQTT");

        if (client.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD)) {
            Serial.println("connected!");
            lcd.setCursor(0, 1);
            lcd.print("MQTT Connected");
            delay(1000);
            lcd.clear();
        } else {
            lcd.setCursor(0, 1);
            lcd.print("MQTT Failed");
            delay(5000);
        }
    }
}

bool readSensorWithRetry(float& co2, float& temperature, float& humidity, int maxRetries) {
    for (int i = 0; i < maxRetries; i++) {
        if (scd30.blockingReadMeasurementData(co2, temperature, humidity) == 0) {
            return true;
        }
        delay(500);
    }
    return false;
}

bool publishWithRetry(const char* topic, const char* payload, int maxRetries) {
    for (int i = 0; i < maxRetries; i++) {
        if (client.publish(topic, payload, true)) {
            return true;
        }
        delay(1000);
    }
    return false;
}

void smtpCallback(SMTP_Status status) {
    Serial.print("Email status: ");
    Serial.println(status.info());

    if (status.success()) {
        Serial.println("Email sent successfully");
    }
}

void checkCO2Alert(float co2, float temperature, float humidity) {
    if (co2 >= CO2_ALERT_THRESHOLD) {
        unsigned long currentTime = millis();

        if (!emailAlertSent || (currentTime - lastEmailSent >= EMAIL_COOLDOWN)) {
            sendEmailAlert(co2, temperature, humidity);
            lastEmailSent = currentTime;
            emailAlertSent = true;

            lcd.setRGB(255, 0, 0);
            delay(1000);
            lcd.setRGB(255, 255, 255);
        }
    } else {
        if (co2 < CO2_ALERT_THRESHOLD - 100) {
            emailAlertSent = false;
        }
    }
}

void sendEmailAlert(float co2, float temperature, float humidity) {

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected, cannot send email");
        return;
    }

    Serial.println("Preparing to send email alert...");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Sending Alert...");

    ESP_Mail_Session session;
    session.server.host_name = SMTP_HOST;
    session.server.port = SMTP_PORT;
    session.login.email = EMAIL_SENDER;
    session.login.password = EMAIL_SENDER_PASSWORD;
    session.login.user_domain = "";

    session.time.ntp_server = "pool.ntp.org,time.nist.gov";
    session.time.gmt_offset = 1;
    session.time.day_light_offset = 0;

    SMTP_Message message;

    message.sender.name = "ESP8266 CO2 Monitor";
    message.sender.email = EMAIL_SENDER;
    message.subject = "ALERTE: Niveau de CO2 eleve detecte";

    message.addRecipient("Recipient 1", EMAIL_RECIPIENT_1);
    message.addRecipient("Recipient 2", EMAIL_RECIPIENT_2);

    String emailBody =
        "ALERTE - Qualite de l'air degradee\n\n"
        "Le niveau de CO2 a depasse le seuil d'alerte.\n\n"
        "Donnees actuelles:\n"
        "- CO2: " + String(co2, 1) + " ppm\n"
        "- Temperature: " + String(temperature, 1) + " °C\n"
        "- Humidite: " + String(humidity, 1) + " %\n\n"
        "Seuil d'alerte: " + String(CO2_ALERT_THRESHOLD) + " ppm\n\n"
        "Veuillez aerer la salle immediatement.\n";

    message.text.content = emailBody.c_str();
    message.text.charSet = "utf-8";
    message.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;
    message.priority = esp_mail_smtp_priority::esp_mail_smtp_priority_high;

    if (!smtp.connect(&session)) {
        Serial.println("Connection error");
        lcd.clear();
        lcd.print("Email Error!");
        return;
    }

    if (!MailClient.sendMail(&smtp, &message)) {
        Serial.println("Error sending Email");
        lcd.clear();
        lcd.print("Email Failed!");
    } else {
        Serial.println("Email sent successfully");
        lcd.clear();
        lcd.print("Alert Sent!");
        delay(2000);
        lcd.clear();
    }
}
