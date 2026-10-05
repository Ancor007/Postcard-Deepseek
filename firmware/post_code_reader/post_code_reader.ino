#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

namespace cfg {
const char* kWifiSsid = "Wifi";
const char* kWifiPassword = "wifi_pass";
const char* kYandexApiKey = "***";
const char* kYandexProjectId = "***";
const char* kYandexPromptId = "***";
const char* kApiHost = "ai.api.cloud.yandex.net";
constexpr int kApiPort = 443;
const char* kPromptTemplate =
   "Input data: board model={{BOARD_MODEL}}, POST code={{POST_CODE}}.";
}  // namespace cfg

namespace post_reader {
const int kSegPins[7] = {32, 33, 25, 26, 27, 14, 12};
const int kDigPins[4] = {4,5, 17, 16};
constexpr bool kActiveDigitLevel = LOW;
constexpr bool kActiveSegmentLevel = HIGH;
constexpr uint32_t kReadWindowUs = 5000;
constexpr uint32_t kFinalizeTimeoutMs = 10000;
constexpr uint32_t kSignalLostMs = 1000;
constexpr uint32_t kMinStableMs = 300;
}  // namespace post_reader

WebServer server(80);

String g_currentCode = "----";
String g_finalCode = "----";
String g_assemblingCode = "????";
String g_lastStableCode = "NONE";
uint32_t g_lastChangeMs = 0;
uint32_t g_lastSignalMs = 0;
uint32_t g_codeStartMs = 0;
bool g_captureStopped = false;
String g_lastBoardModel = "";

String escapeForJson(const String& src) {
  String out;
  out.reserve(src.length() + 16);
  for (size_t i = 0; i < src.length(); ++i) {
    const char c = src[i];
    if (c == '\"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c >= 32) out += c;
  }
  return out;
}

String fillTemplate(String tpl, const String& boardModel, const String& postCode) {
  tpl.replace("{{BOARD_MODEL}}", boardModel);
  tpl.replace("{{POST_CODE}}", postCode);
  return tpl;
}

String escapeHtml(const String& src) {
  String out;
  out.reserve(src.length() + 32);
  for (size_t i = 0; i < src.length(); ++i) {
    const char c = src[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '\"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

String toDisplayTitle(const String& key) {
  if (key == "previous_stage") return "Previous stage";
  if (key == "current_stage") return "Current stage";
  if (key == "transition_analysis") return "Transition analysis";
  if (key == "likely_causes") return "Likely causes";
  if (key == "first_checks_to_perform") return "First checks to perform";
  if (key == "verdict") return "Verdict";
  return key;
}

String sectionValue(const String& text, const String& key) {
  const String marker = key + ":";
  const int start = text.indexOf(marker);
  if (start < 0) return "";
  const int valueStart = start + marker.length();
  int next = text.length();

  const char* keys[] = {
      "verdict:", "previous_stage:", "current_stage:",
      "transition_analysis:", "likely_causes:", "first_checks_to_perform:"};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    const String k = keys[i];
    const int pos = text.indexOf(k, valueStart);
    if (pos >= 0 && pos < next) next = pos;
  }

  String value = text.substring(valueStart, next);
  value.trim();
  return value;
}

String buildFormattedResultHtml(const String& rawResult) {
  String result = rawResult;
  result.replace("\r\n", "\n");

  if (result.length() == 0) {
    return "<div class='note'>Пока нет результата анализа.</div>";
  }

  // Hide technical/internal errors from end users.
  if (result.indexOf("Yandex API request failed") >= 0 ||
      result.indexOf("Yandex API response does not contain JSON") >= 0 ||
      result.indexOf("Failed to parse Yandex response") >= 0 ||
      result.indexOf("Failed to connect to Yandex API") >= 0) {
    return "<div class='note'>Не удалось получить ответ модели. Проверьте подключение и повторите попытку.</div>";
  }

  const char* orderedKeys[] = {
      "verdict", "previous_stage", "current_stage",
      "transition_analysis", "likely_causes", "first_checks_to_perform"};

  String html;
  String recognizedSegments;

  for (size_t i = 0; i < sizeof(orderedKeys) / sizeof(orderedKeys[0]); ++i) {
    const String key = orderedKeys[i];
    const String value = sectionValue(result, key);
    if (value.length() == 0) continue;

    if (recognizedSegments.length() > 0) recognizedSegments += "\n";
    recognizedSegments += key + ": " + value;

    String block = escapeHtml(value);
    block.replace("\n", "<br>");
    html += "<div class='result-block'><div class='result-title'>" + toDisplayTitle(key) +
            "</div><div class='result-text'>" + block + "</div></div>";
  }

  String notes = result;
  if (recognizedSegments.length() > 0) {
    notes = "";
    int cursor = 0;
    while (cursor < result.length()) {
      int closestPos = result.length();
      String closestKey = "";
      for (size_t i = 0; i < sizeof(orderedKeys) / sizeof(orderedKeys[0]); ++i) {
        String marker = String(orderedKeys[i]) + ":";
        int p = result.indexOf(marker, cursor);
        if (p >= 0 && p < closestPos) {
          closestPos = p;
          closestKey = orderedKeys[i];
        }
      }
      if (closestPos == result.length()) {
        String tail = result.substring(cursor);
        tail.trim();
        if (tail.length() > 0) {
          if (notes.length() > 0) notes += "\n";
          notes += tail;
        }
        break;
      }
      if (closestPos > cursor) {
        String before = result.substring(cursor, closestPos);
        before.trim();
        if (before.length() > 0) {
          if (notes.length() > 0) notes += "\n";
          notes += before;
        }
      }
      String value = sectionValue(result, closestKey);
      int consumed = result.indexOf(String(closestKey) + ": " + value, closestPos);
      if (consumed < 0) {
        cursor = closestPos + closestKey.length() + 1;
      } else {
        cursor = consumed + closestKey.length() + 2 + value.length();
      }
    }
  }

  notes.trim();
  if (notes.length() > 0 && notes != result) {
    String notesHtml = escapeHtml(notes);
    notesHtml.replace("\n", "<br>");
    html += "<div class='result-block'><div class='result-title'>Примечания</div><div class='result-text'>" +
            notesHtml + "</div></div>";
  }

  if (html.length() == 0) {
    String plain = escapeHtml(result);
    plain.replace("\n", "<br>");
    html = "<div class='result-block'><div class='result-title'>Ответ</div><div class='result-text'>" +
           plain + "</div></div>";
  }

  return html;
}

int decodeDigit(const bool seg[7]) {
  uint8_t mask = 0;
  for (int i = 0; i < 7; i++) {
    if (seg[i]) mask |= (1 << i);
  }
  switch (mask) {
    case 0b00111111: return 0;
    case 0b00000110: return 1;
    case 0b01011011: return 2;
    case 0b01001111: return 3;
    case 0b01100110: return 4;
    case 0b01101101: return 5;
    case 0b01111101: return 6;
    case 0b00000111: return 7;
    case 0b01111111: return 8;
    case 0b01101111: return 9;
    case 0b01110111: return 10;  // A
    case 0b01111100: return 11;  // b
    case 0b00111001: return 12;  // C
    case 0b01011110: return 13;  // d
    case 0b01111001: return 14;  // E
    case 0b01110001: return 15;  // F
    default: return -1;
  }
}

String readRawPostCode() {
  int digits[4] = {-1, -1, -1, -1};
  const uint32_t started = micros();

  while (micros() - started < post_reader::kReadWindowUs) {
    for (int d = 0; d < 4; d++) {
      if (digitalRead(post_reader::kDigPins[d]) == post_reader::kActiveDigitLevel) {
        bool seg[7];
        for (int s = 0; s < 7; s++) {
          seg[s] = (digitalRead(post_reader::kSegPins[s]) == post_reader::kActiveSegmentLevel);
        }
        digits[d] = decodeDigit(seg);
      }
    }
  }

  String out;
  bool hasValue = false;
  for (int i = 0; i < 4; i++) {
    if (digits[i] >= 0) {
      hasValue = true;
      if (digits[i] == 10) out += "A";
      else if (digits[i] == 11) out += "b";
      else if (digits[i] == 12) out += "C";
      else if (digits[i] == 13) out += "d";
      else if (digits[i] == 14) out += "E";
      else if (digits[i] == 15) out += "F";
      else out += String(digits[i]);
    } else {
      out += "?";
    }
  }
  return hasValue ? out : "";
}

void mergeFrame(const String& frame) {
  for (int i = 0; i < 4; i++) {
    if (frame[i] != '?') g_assemblingCode[i] = frame[i];
  }
}

bool isFullCode(const String& value) {
  return value.indexOf('?') == -1;
}

void pollPostCode() {
  if (g_captureStopped) return;

  const String raw = readRawPostCode();
  const uint32_t now = millis();

  if (raw.length() > 0) {
    g_lastSignalMs = now;
    mergeFrame(raw);

    if (isFullCode(g_assemblingCode) && g_assemblingCode != g_currentCode) {
      if (g_currentCode != "----" && (now - g_codeStartMs) >= post_reader::kMinStableMs) {
        g_lastStableCode = g_currentCode;
      }
      g_currentCode = g_assemblingCode;
      g_assemblingCode = "????";
      g_codeStartMs = now;
      g_lastChangeMs = now;

      Serial.print("POST: ");
      Serial.println(g_currentCode);
    }
  }

  if (g_currentCode != "----" && (now - g_lastChangeMs) >= post_reader::kFinalizeTimeoutMs) {
    g_captureStopped = true;
    g_finalCode = g_currentCode;
    Serial.print("FINAL POST CODE: ");
    Serial.println(g_finalCode);
    return;
  }

  if ((now - g_lastSignalMs) >= post_reader::kSignalLostMs && g_currentCode != "----") {
    g_captureStopped = true;
    Serial.print("POST interrupted. Last stable: ");
    Serial.println(g_lastStableCode);
  }
}

String findFirstUsefulText(JsonVariantConst node) {
  if (node.is<const char*>()) {
    const char* s = node.as<const char*>();
    if (s && s[0] != '\0') return String(s);
    return "";
  }

  if (node.is<JsonObjectConst>()) {
    JsonObjectConst obj = node.as<JsonObjectConst>();

    // Preferred keys first.
    const char* preferredKeys[] = {"output_text", "text", "result", "content", "message"};
    for (size_t i = 0; i < sizeof(preferredKeys) / sizeof(preferredKeys[0]); ++i) {
      const char* key = preferredKeys[i];
      if (!obj[key].isNull()) {
        String nested = findFirstUsefulText(obj[key]);
        if (nested.length() > 0) return nested;
      }
    }

    for (JsonPairConst kv : obj) {
      String nested = findFirstUsefulText(kv.value());
      if (nested.length() > 0) return nested;
    }
    return "";
  }

  if (node.is<JsonArrayConst>()) {
    JsonArrayConst arr = node.as<JsonArrayConst>();
    for (JsonVariantConst item : arr) {
      String nested = findFirstUsefulText(item);
      if (nested.length() > 0) return nested;
    }
    return "";
  }

  return "";
}

String extractContentFromResponse(const String& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    return "Failed to parse Yandex response.";
  }

  if (doc["output"].is<JsonArrayConst>()) {
    JsonArrayConst output = doc["output"].as<JsonArrayConst>();

    // Prefer final assistant message text over reasoning summary.
    for (JsonObjectConst item : output) {
      const char* type = item["type"] | "";
      const char* role = item["role"] | "";
      const char* status = item["status"] | "";
      if (String(type) == "message" && String(role) == "assistant" &&
          (String(status) == "completed" || String(status).length() == 0)) {
        if (item["content"].is<JsonArrayConst>()) {
          JsonArrayConst content = item["content"].as<JsonArrayConst>();
          for (JsonObjectConst part : content) {
            const char* partType = part["type"] | "";
            if (String(partType) == "output_text" || String(partType).length() == 0) {
              const char* text = part["text"] | "";
              if (text[0] != '\0') return String(text);
            }
          }
        }
      }
    }
  }

  if (doc["output_text"].is<const char*>()) {
    const char* text = doc["output_text"];
    if (text && text[0] != '\0') return String(text);
  }
  if (doc["result"].is<const char*>()) {
    const char* text = doc["result"];
    if (text && text[0] != '\0') return String(text);
  }
  if (doc["choices"][0]["message"]["content"].is<const char*>()) {
    const char* content = doc["choices"][0]["message"]["content"];
    if (content && content[0] != '\0') return String(content);
  }
  if (doc["response"]["output_text"].is<const char*>()) {
    const char* text = doc["response"]["output_text"];
    if (text && text[0] != '\0') return String(text);
  }
  if (doc["output"][0]["content"][0]["text"].is<const char*>()) {
    const char* text = doc["output"][0]["content"][0]["text"];
    if (text && text[0] != '\0') return String(text);
  }
  if (doc["output"][0]["content"][0]["output_text"].is<const char*>()) {
    const char* text = doc["output"][0]["content"][0]["output_text"];
    if (text && text[0] != '\0') return String(text);
  }
  if (doc["output"][0]["text"].is<const char*>()) {
    const char* text = doc["output"][0]["text"];
    if (text && text[0] != '\0') return String(text);
  }
  if (doc["message"]["text"].is<const char*>()) {
    const char* text = doc["message"]["text"];
    if (text && text[0] != '\0') return String(text);
  }
  if (doc["result"]["text"].is<const char*>()) {
    const char* text = doc["result"]["text"];
    if (text && text[0] != '\0') return String(text);
  }

  String anyText = findFirstUsefulText(doc.as<JsonVariantConst>());
  if (anyText.length() > 0) return anyText;

  Serial.println("Yandex response JSON parsed, but text fields not found. Full JSON:");
  serializeJsonPretty(doc, Serial);
  Serial.println();
  return "Yandex returned empty content.";
}

String readHttpJsonBody(const String& response) {
  int start = response.indexOf('{');
  int end = response.lastIndexOf('}');

  if (start < 0 || end < 0 || end < start) {
    start = response.indexOf('[');
    end = response.lastIndexOf(']');
  }
  if (start < 0 || end < 0 || end < start) {
    return "";
  }
  return response.substring(start, end + 1);
}

int readHttpStatusCode(const String& response) {
  const int firstSpace = response.indexOf(' ');
  if (firstSpace < 0) return -1;
  const int secondSpace = response.indexOf(' ', firstSpace + 1);
  if (secondSpace < 0) return -1;
  return response.substring(firstSpace + 1, secondSpace).toInt();
}

String readHttpBody(const String& response) {
  const String sep = "\r\n\r\n";
  const int bodyStart = response.indexOf(sep);
  if (bodyStart < 0) return "";
  return response.substring(bodyStart + sep.length());
}

bool isHexChunkLine(const String& line) {
  if (line.length() == 0 || line.length() > 8) return false;
  for (size_t i = 0; i < line.length(); ++i) {
    const char c = line[i];
    const bool isHex =
        (c >= '0' && c <= '9') ||
        (c >= 'a' && c <= 'f') ||
        (c >= 'A' && c <= 'F');
    if (!isHex) return false;
  }
  return true;
}

String stripChunkedArtifacts(const String& body) {
  String cleaned;
  cleaned.reserve(body.length());

  int start = 0;
  while (start < body.length()) {
    int end = body.indexOf('\n', start);
    if (end < 0) end = body.length();
    String line = body.substring(start, end);
    line.trim();

    // Remove chunked transfer size lines like "12c", "330", "0".
    if (!isHexChunkLine(line)) {
      cleaned += body.substring(start, end);
      if (end < body.length()) cleaned += '\n';
    }

    start = end + 1;
  }

  return cleaned;
}

String callYandex(const String& inputMessage, const String& boardModel, const String& postCode) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(20000);

  if (!client.connect(cfg::kApiHost, cfg::kApiPort)) {
    return "Failed to connect to Yandex API.";
  }

  JsonDocument payloadDoc;
  JsonObject prompt = payloadDoc["prompt"].to<JsonObject>();
  prompt["id"] = cfg::kYandexPromptId;
  JsonObject vars = prompt["variables"].to<JsonObject>();
  vars["BOARD_MODEL"] = boardModel;
  vars["POST_CODE"] = postCode;
  payloadDoc["input"] = inputMessage;

  String payload;
  serializeJson(payloadDoc, payload);

  String req = String("POST /v1/responses HTTP/1.1\r\n") +
               "Host: " + cfg::kApiHost + "\r\n" +
               "Accept: application/json\r\n" +
               "Authorization: Api-Key " + String(cfg::kYandexApiKey) + "\r\n" +
               "OpenAI-Project: " + String(cfg::kYandexProjectId) + "\r\n" +
               "Content-Type: application/json\r\n" +
               "Connection: close\r\n" +
               "Content-Length: " + payload.length() + "\r\n\r\n" +
               payload;
  client.print(req);

  String response;
  while (client.connected() || client.available()) {
    if (client.available()) {
      response += client.readStringUntil('\n');
      response += '\n';
    }
  }

  const int status = readHttpStatusCode(response);
  if (status < 200 || status >= 300) {
    Serial.print("Yandex API HTTP status: ");
    Serial.println(status);
    const String bodyRaw = readHttpBody(response);
    if (bodyRaw.length() > 0) {
      Serial.println("Yandex API response body:");
      Serial.println(bodyRaw);
    } else {
      Serial.println("Yandex API response body is empty.");
    }
    return "Yandex API request failed. See Serial for details.";
  }

  const String bodyRaw = readHttpBody(response);
  const String bodyClean = stripChunkedArtifacts(bodyRaw);
  const String body = readHttpJsonBody(bodyClean);
  if (body.isEmpty()) {
    return "Yandex API response does not contain JSON.";
  }
  return extractContentFromResponse(body);
}

String renderPage(const String& result = "", const String& boardModel = "", const String& postCode = "") {
  const String effectiveBoard = boardModel.isEmpty() ? g_lastBoardModel : boardModel;
  const String effectivePost = postCode.isEmpty()
                                   ? ((g_currentCode == "----") ? "" : g_currentCode)
                                   : postCode;

  String html;
  html.reserve(7000);
  const String formattedResultHtml = buildFormattedResultHtml(result);
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>POST Analyzer</title><style>";
  html += "body{font:16px Arial,sans-serif;max-width:720px;margin:0 auto;padding:16px;background:#f6f7fb;color:#222}";
  html += "h2{margin:0 0 8px}label{display:block;margin-top:8px;font-weight:600;font-size:14px}";
  html += "input,button{width:100%;padding:10px;margin-top:6px;border:1px solid #ccc;border-radius:8px}";
  html += "button{background:#2563eb;color:#fff;border:0;cursor:pointer}small{color:#666;display:block;margin:6px 0}";
  html += ".result-wrap{margin-top:14px}.result-block{border:1px solid #ddd;border-radius:8px;padding:10px;margin-top:8px;background:#fff}";
  html += ".result-title{font-weight:700;margin-bottom:4px}.result-text{line-height:1.4}.note{padding:10px;border-radius:8px;background:#eef2ff}";
  html += "</style></head><body>";
  html += "<h2>POST Analyzer (ESP32 + Yandex)</h2>";
  html += "<small>Current POST: <b>" + g_currentCode + "</b> | Last stable: <b>" + g_lastStableCode + "</b> | Final POST: <b>" + g_finalCode + "</b></small>";
  html += "<form method='POST' action='/analyze'>";
  html += "<label>Motherboard model</label>";
  html += "<input name='board' placeholder='e.g. ASUS H610M-K' value='" + effectiveBoard + "' required>";
  html += "<label>POST code</label>";
  html += "<input name='post' placeholder='e.g. A2 or 55' value='" + effectivePost + "'>";
  html += "<small>If empty, current captured code will be used: " + g_currentCode + "</small>";
  html += "<button type='submit'>Analyze</button></form>";
  html += "<div class='result-wrap'><label>Diagnostic result</label>" + formattedResultHtml + "</div>";
  html += "</body></html>";
  return html;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", renderPage());
}

void handleAnalyze() {
  const String board = server.arg("board");
  String post = server.arg("post");

  if (board.isEmpty()) {
    server.send(400, "text/plain; charset=utf-8", "Field 'board' is required.");
    return;
  }
  if (post.isEmpty()) {
    post = g_currentCode;
  }
  if (post.isEmpty() || post == "----") {
    server.send(400, "text/plain; charset=utf-8", "POST code not available. Enter it manually.");
    return;
  }

  g_lastBoardModel = board;

  const String promptTemplate = cfg::kPromptTemplate;
  const String fullPrompt = fillTemplate(promptTemplate, board, post);
  const String answer = callYandex(
      fullPrompt,
      board,
      post);
  server.send(200, "text/html; charset=utf-8", renderPage(answer, board, post));
}

void handleApiAnalyze() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"empty body\"}");
    return;
  }

  JsonDocument reqDoc;
  if (deserializeJson(reqDoc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  String board = "";
  String post = "";
  if (reqDoc["board"].is<const char*>()) {
    board = String((const char*)reqDoc["board"]);
  }
  if (reqDoc["post"].is<const char*>()) {
    post = String((const char*)reqDoc["post"]);
  }
  if (post.isEmpty()) post = g_currentCode;

  if (board.isEmpty() || post.isEmpty() || post == "----") {
    server.send(400, "application/json", "{\"error\":\"board and post are required\"}");
    return;
  }

  g_lastBoardModel = board;

  const String promptTemplate = cfg::kPromptTemplate;
  const String fullPrompt = fillTemplate(promptTemplate, board, post);
  const String answer = callYandex(
      fullPrompt,
      board,
      post);

  String body = "{\"board\":\"" + escapeForJson(board) + "\",\"post\":\"" + escapeForJson(post) +
                "\",\"result\":\"" + escapeForJson(answer) + "\"}";
  server.send(200, "application/json", body);
}

bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg::kWifiSsid, cfg::kWifiPassword);
  Serial.print("Connecting WiFi");
  for (int i = 0; i < 40; ++i) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected");
      Serial.print("Open: http://");
      Serial.println(WiFi.localIP());
      return true;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi failed");
  return false;
}

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 7; i++) pinMode(post_reader::kSegPins[i], INPUT);
  for (int i = 0; i < 4; i++) pinMode(post_reader::kDigPins[i], INPUT);

  if (!connectWifi()) {
    return;
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/analyze", HTTP_POST, handleAnalyze);
  server.on("/api/analyze", HTTP_POST, handleApiAnalyze);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  pollPostCode();
}