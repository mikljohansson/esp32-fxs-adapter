#include <Arduino.h>
#include <ESP32Ping.h>
#include <UniversalTelegramBot.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WiFiMulti.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include <driver/dac.h>
#include <DacESP32.h>

#include "config.h"
#include "preference_set.h"
#include "signal.h"

static WiFiMulti wifiMulti;

static WiFiClientSecure botWifiClient;
static UniversalTelegramBot bot(EFA_TELEGRAM_TOKEN, botWifiClient);

// Timestamp when last messages scan was done
static unsigned long lastMessageScan = 0;

// Number of milliseconds between scanning for new Telegram messages
static double scanInterval = EFA_MIN_TELEGRAM_SCAN_INTERVAL;

// Timestamp when current alarm was triggered, 0 if no alarm triggered
static unsigned long lastAlarmTriggered = 0;

// Timestamp when current alarm started the reset cycle, 0 if no alarm triggered
static unsigned long lastAlarmReset = 0;

// Flag indicating that the alarm was handled
static boolean alarmHandled = false;

// Persistent eeprom storage
Preferences preferences;
PreferenceSet chatIds(preferences, "cids");
PreferenceSet authIds(preferences, "aids");

// Current line voltage
static float lineVoltage = 0.0;

void showLED(boolean, boolean, boolean);
void handleNewMessages(int numNewMessages);
void handleAlarm();

// Tone transmit state
static DacESP32 dac(EFA_LINE_TRANSMIT_CHANNEL);

enum TransmitterState {
    NONE,
    DIALTONE
};

void setTransmitterState(TransmitterState state);

template <typename... T>
void serialPrint(const char *message, T... args) {
    int len = snprintf(NULL, 0, message, args...);
    if (len) {
        char buf[len];
        sprintf(buf, message, args...);
        Serial.print(buf);
    }
}

void setup() {
    // Status LED's
    pinMode(EFA_ERROR_LED_PIN, OUTPUT);
    pinMode(EFA_WARN_LED_PIN, OUTPUT);
    pinMode(EFA_OK_LED_PIN, OUTPUT);
    showLED(false, true, false);

    // Line receiving
    adcAttachPin(EFA_LINE_RECEIVE_PIN);

    // Line transmitting
    setTransmitterState(TransmitterState::NONE);

    Serial.begin(115200);
    while (!Serial);

    Serial.println("Starting up");
    Serial.printf("Core %d, clock %d MHz\n", xPortGetCoreID(), getCpuFrequencyMhz());
    Serial.printf("  XTAL %d MHz, APB %d MHz\n\n", getXtalFrequencyMhz(), getApbFrequency() / 1000000);

    preferences.begin("efa-v1", false);

    // Connect to Wifi
    for (auto network : wifiNetworks) {
        wifiMulti.addAP(network.ssid, network.password);
        serialPrint("Added WiFi AP: %s %s\n", network.ssid, network.password);
    }

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(EFA_HOSTNAME);
    WiFi.setAutoReconnect(true);

    // https://github.com/espressif/esp-idf/issues/1366#issuecomment-569377207
    WiFi.persistent(false);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;

    while (true) {
        if (wifiMulti.run() == WL_CONNECTED) {
            break;
        }

        Serial.print(".");
        delay(1000);
    }
    delay(500);

    IPAddress ip = WiFi.localIP();
    serialPrint("\nHostname: %s\n", WiFi.getHostname());
    serialPrint("IP: %s\n", ip.toString().c_str());
    serialPrint("Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
    serialPrint("DNS: %s\n\n", WiFi.dnsIP(0).toString().c_str());

    // Send a ping to the router
    bool ret = Ping.ping(WiFi.gatewayIP(), 1);
    delay(500);
    Serial.println(ret ? "Internet gateway was reachable" : "Not able to reach internet gateway");

    // Use NTP to configure local time
    Serial.print("Retrieving time: ");
    configTime(0, 0, "pool.ntp.org");
    time_t now = time(nullptr);
    while (now < 24 * 3600) {
        Serial.print(".");
        delay(100);
        now = time(nullptr);
    }
    Serial.println(now);

    // Add root certificate for api.telegram.org
    botWifiClient.setCACert(TELEGRAM_CERTIFICATE_ROOT);
    
    showLED(false, false, true);
}

void loop() {
    if (millis() - lastMessageScan > scanInterval) {
        int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
        
        // Apply exponential backoff when no messages were found
        scanInterval = numNewMessages 
            ? EFA_MIN_TELEGRAM_SCAN_INTERVAL
            : std::min(scanInterval * 1.001, (double)EFA_MAX_TELEGRAM_SCAN_INTERVAL);

        while (numNewMessages) {
            handleNewMessages(numNewMessages);
            numNewMessages = bot.getUpdates(bot.last_message_received + 1);
        }

        lastMessageScan = millis();
        serialPrint("Line voltage: %.02f\n", lineVoltage);
    }

    lineVoltage = (float)analogReadMilliVolts(EFA_LINE_RECEIVE_PIN) / 1000;

    // Detect alarm state
    if (lineVoltage >= EFA_ALARM_VOLTAGE) {
        if (lastAlarmTriggered == 0) {
            lastAlarmTriggered = millis();
        }

        if (millis() - lastAlarmTriggered >= EFA_DIALTONE_DELAY) {
            setTransmitterState(TransmitterState::DIALTONE);
        }
        
        // Trigger alarm notifications
        if (millis() - lastAlarmTriggered >= EFA_ALERT_DURATION && !alarmHandled) {
            handleAlarm();
            alarmHandled = true;
        }

        lastAlarmReset = 0;
        showLED(true, false, true);
    }
    else {
        if (alarmHandled) {
            if (lastAlarmReset == 0) {
                lastAlarmReset = millis();
            }

            // Reset alarm notification state
            if (millis() - lastAlarmReset >= EFA_RESET_DURATION) {
                lastAlarmReset = 0;
                alarmHandled = false;
                setTransmitterState(TransmitterState::NONE);
                serialPrint("The alarm notification state was reset\n");
            }
        }

        lastAlarmTriggered = 0;
        showLED(false, false, true);
    }

    delay(10);
}

void handleNewMessages(int numNewMessages) {
    serialPrint("Got %d new messages\n", numNewMessages);

    for (int i = 0; i < numNewMessages; i++) {
        String chatId = bot.messages[i].chat_id;
        String text = bot.messages[i].text;

        String senderName = bot.messages[i].from_name;
        if (senderName == "") {
            senderName = "Guest";
        }

        if (text == "/start") {
            String message = "Welcome to Bihusets larm " + senderName + ".\n\n";
            message += "/login <password> : authenticate before sending commands\n";
            message += "/subscribe : to notify me if the alarm triggers\n";
            message += "/unsubscribe : to stop receiving alarms\n";
            message += "/list : to list subscribed users\n\n";
            message += "/clear : to clear subscribed users\n";
            message += "/debug : show debug information\n";
            bot.sendMessage(chatId, message, "Markdown");
        }

        if (text.startsWith("/login") && text.length() > 7) {
            if (text.substring(7) == EFA_BOT_PASSWORD) {
                authIds.add(chatId.c_str());
                bot.sendMessage(chatId, "Successfully authenticated", "Markdown");
            }
            else {
                bot.sendMessage(chatId, "Incorrect password", "Markdown");
            }
        }

        if (authIds.exists(chatId.c_str())) {
            if (text == "/subscribe") {
                chatIds.add(chatId.c_str());
                bot.sendMessage(chatId, "Ok, I'll notify you if the alarm trips", "Markdown");
            }

            if (text == "/unsubscribe") {
                chatIds.remove(chatId.c_str());
                bot.sendMessage(chatId, "I've unsubscribed you from alarms", "Markdown");
            }

            if (text == "/list") {
                String message = "I'm alerting these users in case of an alarm: ";
                for (int i = 0, sz = chatIds.size(); i < sz; i++) {
                    if (i > 0) {
                        message += ", ";
                    }

                    message += chatIds.get(i);
                }

                bot.sendMessage(chatId, message, "Markdown");
            }

            if (text == "/clear") {
                chatIds.clear();
                authIds.clear();
                bot.sendMessage(chatId, "I've removed all authanticated or subscribed users", "Markdown");
            }

            if (text == "/debug") {
                String message = "";
                message += "Line voltage: ";
                message += lineVoltage;
                message += "\n";
                bot.sendMessage(chatId, message, "Markdown");
            }
        }
        else {
            bot.sendMessage(chatId, "Please `/login <password>` before sending commands", "Markdown");
        }
    }
}

void handleAlarm() {
    serialPrint("Alarm was tripped by line voltage %.02f, notifying users\n", lineVoltage);

    String message = "The Bihuset alarm was tripped!";

    for (int i = 0, sz = chatIds.size(); i < sz; i++) {
        String chatId = chatIds.get(i);
        bot.sendMessage(chatId, message, "Markdown");
    }
}

void showLED(boolean error, boolean warn, boolean ok) {
    digitalWrite(EFA_ERROR_LED_PIN, error ? HIGH : LOW);
    digitalWrite(EFA_WARN_LED_PIN, warn ? HIGH : LOW);
    digitalWrite(EFA_OK_LED_PIN, ok ? HIGH : LOW);
}

void setTransmitterState(TransmitterState state) {
    /*
    switch (state) {
        case NONE:
            if (dac.disable() != ESP_OK) {
                Serial.println("Failed to disable DAC");
            }
            break;

        case DIALTONE:
            if (dac.outputCW(EFA_DIALTONE_FREQUENCY, DAC_CW_SCALE_2, DAC_CW_PHASE_0, 32) != ESP_OK) {
                Serial.println("Failed to enable dialtone on DAC");
            }
            break;
    }
    */
}
