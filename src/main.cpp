#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include <esp_log.h>
#include <vector>
#include <string>
#include <string_view>

#include <Arduino.h>

#include <WiFi.h>
#include <esp_wifi.h>

#include <WiFiClientSecure.h>

#include <HTTPClient.h>

enum State : uint8_t
{
    INIT,            // 0
    WIFI_CONNECTED,  // 1
    REQUEST_MADE,    // 2
    REQUEST_PARSED,  // 3
    POST_SUCCEEDED,  // 4
    ENDPOINT_REACHED // 5
};

State stateMachine = State::INIT;

// Hack to access the auto-generated CA bundle from esp-idf
extern const uint8_t x509_crt_imported_bundle_bin_start[] asm("_binary_x509_crt_bundle_start");

constexpr const char *const RAILNET_PORTAL_URL = "https://railnet.oebb.at/en/connecttoweb";

constexpr const char *const SEARCH_STRING = "action=\"https://railnet.oebb.at/";
constexpr const char *const TOKEN_FIELD_ID = "name=\"_token\"";
constexpr const char *const CEID_FIELD_ID = "name=\"_ceid\"";
constexpr const char *const CHECKIT_FIELD_ID = "name=\"checkit\"";
constexpr const char *const FORMTYPE_FIELD_ID = "name=\"form_type\"";

constexpr const char *const VALUE_FIELD_BEGINNING = "value=\"";
constexpr const size_t VALUE_FIELD_BEGINNING_LEN = 7;

std::array<std::string, 10> lineBuffer;
std::string currentLine{};
uint8_t bufferIndex{0};
std::optional<uint8_t> form_start_line_idx;

struct FormInformation
{
    std::optional<std::string> _token = std::nullopt;
    std::optional<std::string> _ceid = std::nullopt;
    std::optional<std::string> checkit = std::nullopt;
    std::optional<std::string> form_type = std::nullopt;
};

FormInformation formInformation;

void clearFormInformation()
{
    formInformation._token.reset();
    formInformation._ceid.reset();
    formInformation.checkit.reset();
    formInformation.form_type.reset();
}

enum ParserState : uint8_t
{
    PARSER_INIT,          // 0
    SEARCH_STRING_FOUND,  // 1
    TOKEN_FIELD_FOUND,    // 2
    TOKEN_VALUE_FOUND,    // 3
    CEID_FIELD_FOUND,     // 4
    CEID_VALUE_FOUND,     // 5
    CHECKIT_FIELD_FOUND,  // 6
    CHECKIT_VALUE_FOUND,  // 7
    FORMTYPE_FIELD_FOUND, // 8
    FORMTYPE_VALUE_FOUND, // 9
    DONE                  // 10
};
ParserState parserState = ParserState::PARSER_INIT;

WiFiClientSecure client;

uint32_t lastFisFetchMillis = 0;
constexpr uint32_t FIS_FETCH_INTERVAL_MS = 10000; // 10 seconds

std::optional<uint32_t> retryTimeout = std::nullopt;

CookieJar cookieJar;

uint32_t lastDebugPrintMillis = 0;

void checkLineBuffer()
{
    const std::string &current_line_in_buffer = lineBuffer[bufferIndex];

    ParserState previousParserState = parserState;

    char c;

    const auto findValueField = [&](std::optional<std::string> &value_ref, ParserState newParserState) -> void
    {
        const auto value_pos = current_line_in_buffer.find(VALUE_FIELD_BEGINNING);
        if (value_pos != std::string::npos)
        {
            // now we need to parse until we get a '"'
            size_t current_position = value_pos + VALUE_FIELD_BEGINNING_LEN;
            std::string value{};

            for (size_t idx = current_position; idx < current_line_in_buffer.size(); ++idx)
            {
                c = current_line_in_buffer[idx];
                if (c == '"')
                {
                    break;
                }
                else
                {
                    value += c;
                }
            }

            Serial.printf("Found value: %s\n", value.c_str());
            value_ref = value;

            parserState = newParserState;
        }
    };

    do
    {
        if (previousParserState != parserState)
        {
            Serial.printf("Parser state changed from %d to %d\n",
                          static_cast<uint8_t>(previousParserState),
                          static_cast<uint8_t>(parserState));
        }

        previousParserState = parserState;

        switch (parserState)
        {
        case ParserState::PARSER_INIT:
            if (current_line_in_buffer.find(SEARCH_STRING) != std::string::npos)
            {
                parserState = ParserState::SEARCH_STRING_FOUND;
                Serial.printf("Found form beginning here: %s", current_line_in_buffer.c_str());
            }
            break;
        case ParserState::SEARCH_STRING_FOUND:
            if (current_line_in_buffer.find(TOKEN_FIELD_ID) != std::string::npos)
            {
                parserState = ParserState::TOKEN_FIELD_FOUND;
                Serial.printf("Found token field here: %s", current_line_in_buffer.c_str());
            }
            break;
        case ParserState::TOKEN_FIELD_FOUND:
        {
            findValueField(formInformation._token, ParserState::TOKEN_VALUE_FOUND);
            break;
        }
        case ParserState::TOKEN_VALUE_FOUND:
            if (current_line_in_buffer.find(CEID_FIELD_ID) != std::string::npos)
            {
                parserState = ParserState::CEID_FIELD_FOUND;
                Serial.printf("Found ceid field here: %s", current_line_in_buffer.c_str());
            }
            break;
        case ParserState::CEID_FIELD_FOUND:
        {
            findValueField(formInformation._ceid, ParserState::CEID_VALUE_FOUND);
            break;
        }
        case ParserState::CEID_VALUE_FOUND:
            if (current_line_in_buffer.find(CHECKIT_FIELD_ID) != std::string::npos)
            {
                parserState = ParserState::CHECKIT_FIELD_FOUND;
                Serial.printf("Found checkit field here: %s", current_line_in_buffer.c_str());
            }
            break;
        case ParserState::CHECKIT_FIELD_FOUND:
        {
            findValueField(formInformation.checkit, ParserState::CHECKIT_VALUE_FOUND);
            break;
        }
        case ParserState::CHECKIT_VALUE_FOUND:
            if (current_line_in_buffer.find(FORMTYPE_FIELD_ID) != std::string::npos)
            {
                parserState = ParserState::FORMTYPE_FIELD_FOUND;
                Serial.printf("Found form_type field here: %s", current_line_in_buffer.c_str());
            }
            break;
        case ParserState::FORMTYPE_FIELD_FOUND:
        {
            findValueField(formInformation.form_type, ParserState::FORMTYPE_VALUE_FOUND);
            break;
        }
        case ParserState::FORMTYPE_VALUE_FOUND:
            Serial.printf("All values were found\n");
            break;
        default:
            break;
        }
    } while (parserState != previousParserState);

    if (formInformation._token && formInformation._ceid && formInformation.checkit && formInformation.form_type && parserState != ParserState::DONE)
    {
        parserState = ParserState::DONE;
        Serial.printf("Form parsing done:\n_token: %s\n_ceid: %s\ncheckit: %s\nform_type: %s\n",
                      formInformation._token->c_str(),
                      formInformation._ceid->c_str(),
                      formInformation.checkit->c_str(),
                      formInformation.form_type->c_str());
        stateMachine = State::REQUEST_PARSED;
    }
}

void parseResponseBufferIntoLineBuffer(char *buf, size_t len)
{
    char c;

    for (size_t pos = 0; pos < len; ++pos)
    {
        c = buf[pos];
        if (c == '\n')
        {
            currentLine += c;
            // Serial.printf("Parsed line: %s", currentLine.c_str());

            lineBuffer[bufferIndex] = currentLine;

            checkLineBuffer();

            bufferIndex = (bufferIndex + 1) % lineBuffer.size();

            currentLine.clear();
        }
        else
        {
            currentLine += c;
        }
    }
}

void setClock()
{
    configTime(0, 0, "pool.ntp.org");

    Serial.print(F("Waiting for NTP time sync: "));

    time_t nowSecs = time(nullptr);

    while (nowSecs < 8 * 3600 * 2)
    {
        delay(500);
        Serial.print(F("."));
        yield();
        nowSecs = time(nullptr);
    }

    Serial.println();

    struct tm timeinfo;
    gmtime_r(&nowSecs, &timeinfo);

    Serial.print(F("Current time: "));

    char buf[26];
    Serial.print(asctime_r(&timeinfo, buf));
}


void readMacAddress()
{
  uint8_t baseMac[6];
  esp_err_t ret = esp_wifi_get_mac(WIFI_IF_STA, baseMac);
  if (ret == ESP_OK) {
    Serial.printf("%02x:%02x:%02x:%02x:%02x:%02x\n",
                  baseMac[0], baseMac[1], baseMac[2],
                  baseMac[3], baseMac[4], baseMac[5]);
    baseMac[4] -= 1;
    esp_wifi_set_mac(WIFI_IF_STA, baseMac);
  } else {
    Serial.println("Failed to read MAC address");
  }
}

void setup()
{
    esp_log_level_set("*", ESP_LOG_ERROR);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("dhcpc", ESP_LOG_INFO);

    Serial.begin(115200);

    for (int i = 0; i < 10; i++)
    {
        Serial.println();
    }

    Serial.println("setup()");

    delay(1000);

    // Add WiFi networks to WiFiMulti
    /*wifiMulti.addAP("OEBB", nullptr);

    Serial.print("Waiting for WiFi to connect...");

    while (wifiMulti.run() != WL_CONNECTED)
    {
        Serial.print(".");
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }*/

    // scan and print wifi list before connecting

    WiFi.begin("OEBB");
    WiFi.mode(WIFI_STA);
    WiFi.persistent(true);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);

    // readMacAddress();

    while (WiFi.status() != WL_CONNECTED)
    {
        Serial.print(".");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    Serial.println(" connected");
    Serial.printf("Connected to WiFi %s: %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

    // setClock();

    stateMachine = State::WIFI_CONNECTED;

    client.setInsecure();
    client.setCACertBundle(x509_crt_imported_bundle_bin_start);
}

void loop()
{
    if (retryTimeout && millis() > *retryTimeout)
    {
        retryTimeout.reset();
        Serial.println("Retrying now...");
        stateMachine = State::WIFI_CONNECTED;
        parserState = ParserState::PARSER_INIT;
    }

    if (lastDebugPrintMillis == 0 || (millis() - lastDebugPrintMillis) > 3000)
    {
        lastDebugPrintMillis = millis();
        Serial.printf("Current state machine state: %d\n", static_cast<uint8_t>(stateMachine));
        Serial.printf("Current parser state: %d\n", static_cast<uint8_t>(parserState));
    }

    switch (stateMachine)
    {
    case State::WIFI_CONNECTED:
    {
        lineBuffer.fill({});
        currentLine.clear();
        bufferIndex = 0;
        form_start_line_idx.reset();
        clearFormInformation();

        parserState = ParserState::PARSER_INIT;
        stateMachine = State::REQUEST_MADE;

        HTTPClient https;
        https.setCookieJar(&cookieJar);

        https.setTimeout(10000);
        https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        if (https.begin(client, RAILNET_PORTAL_URL))
        {
            Serial.print("[HTTP] GET...\n");
            // start connection and send HTTP header
            int httpCode = https.GET();
            if (httpCode > 0)
            {
                // HTTP header has been send and Server response header has been handled
                Serial.printf("[HTTP] GET... code: %d\n", httpCode);

                // file found at server
                if (httpCode == HTTP_CODE_OK)
                {

                    // get length of document (is -1 when Server sends no Content-Length header)
                    int len = https.getSize();

                    // create buffer for read
                    char buff[128] = {0};

                    // get tcp stream
                    WiFiClient *stream = https.getStreamPtr();

                    // read all data from server
                    while (https.connected() && (len > 0 || len == -1))
                    {
                        // get available data size
                        size_t size = stream->available();

                        if (size)
                        {
                            // read up to 128 byte
                            int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));

                            parseResponseBufferIntoLineBuffer(buff, c);

                            if (stateMachine == State::REQUEST_PARSED)
                            {
                                https.end();
                                break;
                            }

                            // write it to Serial
                            // Serial.write(buff, c);

                            if (len > 0)
                            {
                                len -= c;
                            }
                        }
                        delay(1);
                    }

                    Serial.println();
                    Serial.print("[HTTP] connection closed or file end.\n");
                }
            }
            else
            {
                Serial.printf("[HTTP] GET... failed, error: %s\n", https.errorToString(httpCode).c_str());
                retryTimeout = millis() + 5000;
                Serial.println("Retrying in 5 seconds...");
            }

            https.end();
        }

        if (stateMachine != State::REQUEST_PARSED)
        {
            Serial.println("Form parsing did not complete successfully.");
            retryTimeout = millis() + 5000;
            Serial.println("Retrying in 5 seconds...");
        }
        break;
    }
    case State::REQUEST_PARSED:
    {
        // send POST request with form data
        if (!formInformation._token || !formInformation._ceid || !formInformation.checkit || !formInformation.form_type)
        {
            Serial.println("Form information incomplete, cannot send POST request");
            break;
        }

        {
            HTTPClient https;
            https.setCookieJar(&cookieJar);

            Serial.println("Sending POST request with form data...");

            https.setTimeout(10000);
            https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

            if (https.begin(client, "https://railnet.oebb.at/en/connecttoweb"))
            {
                https.addHeader("Content-Type", "application/x-www-form-urlencoded");

                String postData = "_token=" + String(formInformation._token->c_str()) +
                                  "&_ceid=" + String(formInformation._ceid->c_str()) +
                                  "&checkit=" + String(formInformation.checkit->c_str()) +
                                  "&form_type=" + String(formInformation.form_type->c_str());

                Serial.printf("POST data: %s\n", postData.c_str());

                int httpCode = https.POST(postData);

                if (httpCode > 0)
                {
                    Serial.printf("[HTTP] POST... code: %d\n", httpCode);

                    if (httpCode == HTTP_CODE_OK)
                    {
                        stateMachine = State::POST_SUCCEEDED;
                    }
                }
                else
                {
                    Serial.printf("[HTTP] POST... failed, error: %s\n", https.errorToString(httpCode).c_str());
                }

                https.end();
            }
        }
        break;
    }
    case State::POST_SUCCEEDED:
    case State::ENDPOINT_REACHED:
        // now we can proceed with collecting data and sending it home
        // we fetch https://railnet.oebb.at/assets/media/fis/combined.json, and then POST it to our endpoint

        if (lastFisFetchMillis == 0 || (millis() - lastFisFetchMillis) > FIS_FETCH_INTERVAL_MS)
        {
            HTTPClient https;

            https.setTimeout(10000);
            https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            https.setCookieJar(&cookieJar);

            lastFisFetchMillis = millis();

            if (https.begin(client, "https://railnet.oebb.at/assets/media/fis/combined.json"))
            {
                Serial.print("[HTTP] GET combined.json...\n");
                int httpCode = https.GET();
                if (httpCode > 0)
                {
                    Serial.printf("[HTTP] GET... code: %d\n", httpCode);

                    if (httpCode == HTTP_CODE_OK)
                    {
                        String payload = https.getString();
                        Serial.printf("combined.json payload: %s\n", payload.c_str());

                        // Now POST to our endpoint
                        HTTPClient httpPost;
                        httpPost.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
                        httpPost.addHeader("X-Api-Key", String(SECRET));

                        Serial.printf("Making POST request to endpoint: %s\n", POST_ENDPOINT_URL);

                        if (httpPost.begin(client, POST_ENDPOINT_URL))
                        {
                            httpPost.addHeader("Content-Type", "application/json");

                            int postCode = httpPost.POST(payload);

                            if (postCode > 0)
                            {
                                Serial.printf("[HTTP] POST to endpoint... code: %d\n", postCode);
                                if (postCode == HTTP_CODE_OK)
                                {
                                    stateMachine = State::ENDPOINT_REACHED;
                                }
                            }
                            else
                            {
                                Serial.printf("[HTTP] POST to endpoint... failed, error: %s\n", httpPost.errorToString(postCode).c_str());
                            }

                            httpPost.end();
                        }
                    }
                }
                else
                {
                    Serial.printf("[HTTP] GET combined.json... failed, error: %s\n", https.errorToString(httpCode).c_str());
                }

                https.end();
            }
            break;
        }
    default:
        break;
    }

    delay(10);
}
