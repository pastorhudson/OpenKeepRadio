//
// Created by pastorhudson on 9/18/2026.
//

#include <Arduino.h>
#include <ESP8266WiFi.h>

#include "AudioFileSourceICYStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

#include <Wire.h>
#include <U8x8lib.h>

// Built-in OLED
// SDA = GPIO5 / D1
// SCL = GPIO4 / D2
U8X8_SSD1306_128X64_NONAME_HW_I2C oled(U8X8_PIN_NONE);

char oledLines[8][17] = {0};

void updateOLED() {
    oled.clearDisplay();

    for (int i = 0; i < 8; i++) {
        oled.drawString(0, i, oledLines[i]);
    }
}

void oledLog(const char *text) {
    // Scroll old lines upward
    for (int i = 0; i < 7; i++) {
        strncpy(oledLines[i], oledLines[i + 1], 16);
        oledLines[i][16] = '\0';
    }

    // Add newest line at bottom
    strncpy(oledLines[7], text, 16);
    oledLines[7][16] = '\0';

    updateOLED();

    // Mirror to Serial too
    Serial.println(text);
}

void logf(const char *format, ...) {
    char buffer[96];

    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    Serial.println(buffer);

    // OLED is only 16 characters wide,
    // so split longer messages across lines.
    const char *p = buffer;

    while (*p) {
        char line[17];

        strncpy(line, p, 16);
        line[16] = '\0';

        // Don't print twice to Serial
        for (int i = 0; i < 7; i++) {
            strncpy(oledLines[i], oledLines[i + 1], 16);
            oledLines[i][16] = '\0';
        }

        strncpy(oledLines[7], line, 16);
        oledLines[7][16] = '\0';

        updateOLED();

        size_t len = strlen(p);

        if (len <= 16)
            break;

        p += 16;
    }
}




// --------------------------------------------------
// Wi-Fi
// --------------------------------------------------

const char *WIFI_SSID = "haas";
const char *WIFI_PASSWORD = "54421912";

// The .m3u points at the actual Icecast mount:
// https://keepradio.quarteredcircle.net/KeepRadio
//
// ESP8266Audio's built-in stream source is HTTP based,
// so we're trying the HTTP endpoint first.
const char *STREAM_URL =
    "http://keepradio.quarteredcircle.net/KeepRadio";


// --------------------------------------------------
// Audio objects
// --------------------------------------------------

AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceICYStream *stream = nullptr;
AudioFileSourceBuffer *buffer = nullptr;
AudioOutputI2S *audioOut = nullptr;


// --------------------------------------------------
// Metadata callback
// --------------------------------------------------

void metadataCallback(
    void *cbData,
    const char *type,
    bool isUnicode,
    const char *string
) {
    (void)cbData;
    (void)isUnicode;

    char typeBuffer[32];
    char valueBuffer[128];

    strncpy_P(typeBuffer, type, sizeof(typeBuffer));
    typeBuffer[sizeof(typeBuffer) - 1] = '\0';

    strncpy_P(valueBuffer, string, sizeof(valueBuffer));
    valueBuffer[sizeof(valueBuffer) - 1] = '\0';

    Serial.printf(
        "METADATA: %s = %s\n",
        typeBuffer,
        valueBuffer
    );
}


// --------------------------------------------------
// Status callback
// --------------------------------------------------

void statusCallback(
    void *cbData,
    int code,
    const char *string
) {
    const char *source =
        reinterpret_cast<const char *>(cbData);

    char message[128];

    strncpy_P(message, string, sizeof(message));
    message[sizeof(message) - 1] = '\0';

    Serial.printf(
        "STATUS [%s] %d: %s\n",
        source,
        code,
        message
    );
}


// --------------------------------------------------
// Stop / clean up stream
// --------------------------------------------------

void stopRadio() {

    if (mp3) {
        if (mp3->isRunning()) {
            mp3->stop();
        }

        delete mp3;
        mp3 = nullptr;
    }

    if (buffer) {
        delete buffer;
        buffer = nullptr;
    }

    if (stream) {
        delete stream;
        stream = nullptr;
    }

    if (audioOut) {
        delete audioOut;
        audioOut = nullptr;
    }
}


// --------------------------------------------------
// Start radio
// --------------------------------------------------

bool startRadio() {

    Serial.println();
    Serial.println("Starting Keep Radio...");
    Serial.printf("URL: %s\n", STREAM_URL);
    Serial.printf(
        "Free heap before stream: %u bytes\n",
        ESP.getFreeHeap()
    );

    stopRadio();

    // ------------------------------------------------
    // Icecast stream
    // ------------------------------------------------

    stream =
        new AudioFileSourceICYStream(STREAM_URL);

    stream->RegisterMetadataCB(
        metadataCallback,
        nullptr
    );

    // Try reconnecting if Wi-Fi hiccups.
    stream->SetReconnect(3, 1000);


    // ------------------------------------------------
    // Network buffer
    // ------------------------------------------------

    buffer =
        new AudioFileSourceBuffer(
            stream,
            8192
        );

    buffer->RegisterStatusCB(
        statusCallback,
        (void *)"buffer"
    );


    // ------------------------------------------------
    // I2S output
    // ------------------------------------------------

    audioOut =
        new AudioOutputI2S();

    // Mix stereo stream to mono.
    // Useful for the mono MAX98357A amplifier.
    audioOut->SetOutputModeMono(true);

    // Start low so we don't get blasted by the speaker.
    audioOut->SetGain(0.20);


    // ------------------------------------------------
    // MP3 decoder
    // ------------------------------------------------

    mp3 =
        new AudioGeneratorMP3();

    mp3->RegisterStatusCB(
        statusCallback,
        (void *)"mp3"
    );

    Serial.printf(
        "Free heap before decoder: %u bytes\n",
        ESP.getFreeHeap()
    );

    bool success =
        mp3->begin(buffer, audioOut);

    if (!success) {

        Serial.println(
            "ERROR: MP3 decoder failed to start."
        );

        return false;
    }

    Serial.println("Radio started.");

    return true;
}


// --------------------------------------------------
// Connect Wi-Fi
// --------------------------------------------------

bool connectWiFi() {

    logf("WiFi: %s", WIFI_SSID);

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );

    unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED) {

        delay(500);

        logf(
            "S:%d R:%d",
            WiFi.status(),
            WiFi.RSSI()
        );

        if (millis() - start > 20000) {

            oledLog("WiFi TIMEOUT");

            logf(
                "Status: %d",
                WiFi.status()
            );

            logf(
                "RSSI: %d",
                WiFi.RSSI()
            );

            return false;
        }
    }

    oledLog("WiFi CONNECTED");

    logf(
        "RSSI: %d dBm",
        WiFi.RSSI()
    );

    IPAddress ip = WiFi.localIP();

    logf(
        "%d.%d.%d.%d",
        ip[0],
        ip[1],
        ip[2],
        ip[3]
    );

    return true;
}


// --------------------------------------------------
// Setup
// --------------------------------------------------

void setup() {

    Serial.begin(115200);

    delay(1000);

    // Built-in OLED
    Wire.begin(5, 4);  // SDA GPIO5, SCL GPIO4

    oled.setI2CAddress(0x3C * 2);
    oled.begin();
    oled.setFont(u8x8_font_chroma48medium8_r);
    oled.clearDisplay();

    oledLog("KEEP RADIO");
    oledLog("Booting...");


    Serial.println();
    Serial.println();
    Serial.println("===========================");
    Serial.println("      KEEP RADIO");
    Serial.println("===========================");

    // ESP8266Audio recommends the ESP8266 run at
    // 160 MHz for MP3 streaming.
    //
    // This is already set in platformio.ini.

    if (!connectWiFi()) {
        Serial.println("Unable to connect to Wi-Fi.");
        return;
    }

    delay(500);

    startRadio();
}


// --------------------------------------------------
// Loop
// --------------------------------------------------

void loop() {

    // Don't even try audio without Wi-Fi.
    if (WiFi.status() != WL_CONNECTED) {
        static unsigned long lastWiFiRetry = 0;

        if (millis() - lastWiFiRetry > 10000) {
            lastWiFiRetry = millis();

            Serial.println("Wi-Fi disconnected. Reconnecting...");
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }

        delay(10);
        return;
    }

    if (mp3 && mp3->isRunning()) {

        if (!mp3->loop()) {
            Serial.println("MP3 stream stopped.");
            mp3->stop();
        }

    } else {

        static unsigned long lastRetry = 0;

        if (millis() - lastRetry > 5000) {
            lastRetry = millis();

            Serial.println("Stream stopped. Restarting...");
            startRadio();
        }
    }

    yield();
}