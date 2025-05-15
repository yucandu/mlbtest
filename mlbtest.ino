#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>

// WiFi credentials
const char* ssid = "mikesnet";
const char* password = "springchicken";

// NTP Server
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -5 * 3600; // Eastern Time (EST+DST)
const int daylightOffset_sec = 3600;   // 1 hour for DST

// MLB Stats API URLs
const char* scheduleUrlTemplate = "http://statsapi.mlb.com/api/v1/schedule?sportId=1&startDate=%s&endDate=%s&teamId=141"; // Blue Jays teamId = 141
const char* gameDataUrlTemplate = "https://statsapi.mlb.com/api/v1.1/game/%s/boxscore";
const char* atBatUrlTemplate = "https://statsapi.mlb.com/api/v1/game/%s/playByPlay";

// Buffer for URL construction
char url[200];

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // Connect to WiFi
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("");
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  // Init and get the time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  printLocalTime();
  
  // Get MLB data
  getMlbData();
}

void loop() {
  // Nothing to do in loop
  delay(10000);
}

void printLocalTime() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return;
  }
  
  Serial.print("Current date & time: ");
  Serial.print(&timeinfo, "%Y-%m-%d %H:%M:%S");
  Serial.println();
}

String getCurrentDate() {
  struct tm timeinfo;
  char dateStr[11]; // YYYY-MM-DD + null terminator
  
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return "2025-05-13"; // Fallback to current date
  }
  
  strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);
  return String(dateStr);
}

String getDateBefore(int daysAgo) {
  struct tm timeinfo;
  char dateStr[11]; // YYYY-MM-DD + null terminator
  
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return "2025-05-11"; // Fallback to date 2 days ago
  }
  
  // Subtract days
  timeinfo.tm_mday -= daysAgo;
  mktime(&timeinfo); // Normalize the time (handles month/year boundaries)
  
  strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);
  return String(dateStr);
}

// Get the latest non-preview game ID
String getLatestGameId() {
  String gameId = "";
  String endDate = getCurrentDate();
  String startDate = getDateBefore(0); // Only check today's games first
  
  // First try to find a live game
  sprintf(url, scheduleUrlTemplate, startDate.c_str(), endDate.c_str());
  Serial.println("Checking for live games today...");
  
  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();
  
  if(httpCode == HTTP_CODE_OK) {
    DynamicJsonDocument filter(128);
    filter["dates"][0]["games"][0]["gamePk"] = true;
    filter["dates"][0]["games"][0]["status"]["abstractGameState"] = true;
    filter["dates"][0]["games"][0]["status"]["detailedState"] = true;

    DynamicJsonDocument doc(768);
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    
    if(!err && doc.containsKey("dates") && doc["dates"].size() > 0) {
      for(JsonVariant game : doc["dates"][0]["games"].as<JsonArray>()) {
        String state = game["status"]["abstractGameState"].as<String>();
        String detailed = game["status"]["detailedState"].as<String>();
        
        // Check for live game first
        if(state == "Live" || detailed.indexOf("In Progress") >= 0) {
          gameId = game["gamePk"].as<String>();
          Serial.println("Found live game: " + gameId);
          http.end();
          return gameId;
        }
      }
    }
  }
  http.end();

  // If no live game found, look for most recent final game
  if(gameId.isEmpty()) {
    startDate = getDateBefore(2); // Check last 2 days for completed games
    sprintf(url, scheduleUrlTemplate, startDate.c_str(), endDate.c_str());
    Serial.println("No live game found, checking recent completed games...");
    
    http.begin(url);
    httpCode = http.GET();
    
    if(httpCode == HTTP_CODE_OK) {
      DynamicJsonDocument filter(128);
      filter["dates"][0]["games"][0]["gamePk"] = true;
      filter["dates"][0]["games"][0]["status"]["abstractGameState"] = true;

      DynamicJsonDocument doc(768);
      DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
      
      if(!err && doc.containsKey("dates") && doc["dates"].size() > 0) {
        // Find most recent final game
        for(JsonVariant date : doc["dates"].as<JsonArray>()) {
          for(JsonVariant game : date["games"].as<JsonArray>()) {
            String state = game["status"]["abstractGameState"].as<String>();
            if(state == "Final") {
              gameId = game["gamePk"].as<String>();
              Serial.println("Found completed game: " + gameId);
              http.end();
              return gameId;
            }
          }
        }
      }
    }
    http.end();
  }
  
  return gameId;
}

// Get the most recent at-bat ID from playByPlay
int getLastAtBatIndex(String gameId) {
  if(gameId.isEmpty()) {
    Serial.println("No game ID provided");
    return -1;
  }
  
  sprintf(url, atBatUrlTemplate, gameId.c_str());
  Serial.print("Getting last at-bat for game: ");
  Serial.println(gameId);
  
  HTTPClient http;
  http.begin(url);
  http.useHTTP10(true);
  int httpCode = http.GET();
  
  if(httpCode == HTTP_CODE_OK) {
    int lastIndex = -1;
    bool inQuotes = false;
    bool foundCurrentPlay = false;
    char searchStr[] = "\"atBatIndex\":";
    char currentPlayStr[] = "\"currentPlay\":";
    int searchLen = strlen(searchStr);
    int currentPlayLen = strlen(currentPlayStr);
    int matchPos = 0;
    int curlyBraceLevel = 0;
    
    size_t len = http.getSize();
    uint8_t buff[128] = {0};
    WiFiClient * stream = http.getStreamPtr();
    
    Serial.println("Parsing response...");
    String numStr = "";
    bool readingNumber = false;
    
    while(http.connected() && (len > 0 || len == -1)) {
      size_t size = stream->available();
      if(size > 0) {
        int readBytes = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
        
        for(int i = 0; i < readBytes; i++) {
          char c = (char)buff[i];
          
          if(c == '{') {
            curlyBraceLevel++;
          } else if(c == '}') {
            curlyBraceLevel--;
            if(curlyBraceLevel == 1) { // Exit currentPlay object if we found index
              if(foundCurrentPlay && lastIndex >= 0) {
                Serial.println("Using currentPlay index");
                http.end();
                return lastIndex;
              }
            }
          }
          
          // Look for currentPlay first
          if(!foundCurrentPlay && c == currentPlayStr[matchPos]) {
            matchPos++;
            if(matchPos == currentPlayLen) {
              foundCurrentPlay = true;
              matchPos = 0;
            }
          } 
          // Then look for atBatIndex
          else if(c == searchStr[matchPos]) {
            matchPos++;
            if(matchPos == searchLen) {
              readingNumber = true;
              numStr = "";
              matchPos = 0;
            }
          } else {
            matchPos = (c == currentPlayStr[0]) ? 1 : 
                      (c == searchStr[0]) ? 1 : 0;
          }
          
          // Read number after match
          if(readingNumber) {
            if(isDigit(c)) {
              numStr += c;
            } else if(numStr.length() > 0) {
              int newIndex = numStr.toInt();
              if(foundCurrentPlay || newIndex > lastIndex) {
                lastIndex = newIndex;
              }
              readingNumber = false;
              
              // If we found currentPlay's atBatIndex, return immediately
              if(foundCurrentPlay) {
                Serial.println("Using currentPlay index");
                http.end();
                return lastIndex;
              }
            }
          }
        }
        
        if(len > 0) {
          len -= readBytes;
        }
      }
    }
    
    http.end();
    if(lastIndex >= 0) {
      Serial.print("Using last at-bat index: ");
      Serial.println(lastIndex);
      return lastIndex;
    }
  }
  
  http.end();
  return -1;
}

// Get pitch data for a specific at-bat
void getPitchDataForAtBat(String gameId, int atBatIndex) {
  if(gameId.isEmpty() || atBatIndex < 0) {
    Serial.println("Invalid game ID or at-bat index");
    return;
  }
  
  sprintf(url, atBatUrlTemplate, gameId.c_str());
  Serial.print("Getting pitch data from: ");
  Serial.println(url);
  
  HTTPClient http;
  http.begin(url);
  http.useHTTP10(true);
  int httpCode = http.GET();
  
  if(httpCode == HTTP_CODE_OK) {
    bool foundTargetAtBat = false;
    bool inPitchData = false;
    bool inAllPlay = false;
    bool inPitch = false;
    bool processedAtBat = false;  // New flag to track if we've already processed this at-bat
    int pitchCount = 0;
    
    size_t len = http.getSize();
    const size_t BUFFER_SIZE = 64;
    uint8_t buff[BUFFER_SIZE] = {0};
    WiFiClient * stream = http.getStreamPtr();
    
    String keyName = "";
    String valueStr = "";
    bool inKey = false;
    bool inValue = false;
    bool inQuotes = false;
    int curlyBraceLevel = 0;
    
    float startSpeed = 0;
    float endSpeed = 0;
    float xCoord = 0;
    float yCoord = 0;
    bool hasStartSpeed = false;
    bool hasEndSpeed = false;
    bool hasXCoord = false;
    bool hasYCoord = false;
    
    Serial.println("Starting to parse pitch data...");
    
    while(http.connected() && (len > 0 || len == -1)) {
      size_t size = stream->available();
      
      if(size > 0) {
        int readBytes = stream->readBytes(buff, ((size > BUFFER_SIZE) ? BUFFER_SIZE : size));
        
        for(int i = 0; i < readBytes; i++) {
          char c = (char)buff[i];
          
          if(c == '{') {
            curlyBraceLevel++;
            if(curlyBraceLevel == 2) {
              inAllPlay = true;
              foundTargetAtBat = false;
            } else if(curlyBraceLevel == 4 && inPitchData && !processedAtBat) {
              inPitch = true;
              hasStartSpeed = hasEndSpeed = hasXCoord = hasYCoord = false;
            }
          } else if(c == '}') {
            if(inPitch && curlyBraceLevel == 4) {
              inPitch = false;
              if(foundTargetAtBat && !processedAtBat && (hasStartSpeed || hasEndSpeed || hasXCoord || hasYCoord)) {
                Serial.print("Pitch #");
                Serial.println(++pitchCount);
                
                if(hasStartSpeed) {
                  Serial.print("Start Speed: ");
                  Serial.print(startSpeed);
                  Serial.println(" mph");
                }
                if(hasEndSpeed) {
                  Serial.print("End Speed: ");
                  Serial.print(endSpeed);
                  Serial.println(" mph");
                }
                if(hasXCoord) {
                  Serial.print("X Coordinate: ");
                  Serial.println(xCoord);
                }
                if(hasYCoord) {
                  Serial.print("Y Coordinate: ");
                  Serial.println(yCoord);
                }
                Serial.println("---");
              }
            }
            curlyBraceLevel--;
            if(curlyBraceLevel == 1) {
              inAllPlay = false;
              if(foundTargetAtBat) {
                processedAtBat = true;  // Mark this at-bat as processed
                foundTargetAtBat = false;
              }
            } else if(curlyBraceLevel == 3) {
              inPitchData = false;
            }
          } else if(c == '\"') {
            inQuotes = !inQuotes;
            if(inQuotes) {
              inKey = true;
              keyName = "";
            } else if(inKey) {
              inKey = false;
              if(keyName == "atBatIndex") {
                inValue = true;
                valueStr = "";
              } else if(keyName == "pitchData") {
                inPitchData = true;
              } else if(inPitchData && (keyName == "startSpeed" || keyName == "endSpeed" || 
                       keyName == "x" || keyName == "y")) {
                inValue = true;
                valueStr = "";
              }
            }
          } else if(inKey && inQuotes) {
            keyName += c;
          } else if(inValue) {
            if((isDigit(c) || c == '.' || c == '-') && valueStr.length() < 10) {
              valueStr += c;
            } else if(c == ',' || c == '}') {
              if(keyName == "atBatIndex" && inAllPlay) {
                if(valueStr.toInt() == atBatIndex && !foundTargetAtBat) {
                  foundTargetAtBat = true;
                  Serial.print("Found target at-bat #");
                  Serial.println(atBatIndex);
                }
              } else if(inPitchData && foundTargetAtBat) {
                if(keyName == "startSpeed") {
                  startSpeed = valueStr.toFloat();
                  hasStartSpeed = true;
                } else if(keyName == "endSpeed") {
                  endSpeed = valueStr.toFloat();
                  hasEndSpeed = true;
                } else if(keyName == "x") {
                  xCoord = valueStr.toFloat();
                  hasXCoord = true;
                } else if(keyName == "y") {
                  yCoord = valueStr.toFloat();
                  hasYCoord = true;
                }
              }
              inValue = false;
            }
          }
        }
        
        if(len > 0) {
          len -= readBytes;
        }
      }
    }
    
    if(pitchCount == 0) {
      Serial.println("No pitch data found for this at-bat");
    }
    
    Serial.println("Done getting pitch data for at-bat.");
  } else {
    Serial.print("HTTP error: ");
    Serial.println(httpCode);
  }
  
  http.end();
}

// Main function to get MLB data
void getMlbData() {
  Serial.print("Time after connecting to wifi: ");
  Serial.println(millis() / 1000.0);
  // 1. Get the latest non-preview game
  String gameId = getLatestGameId();
  
  if(gameId.isEmpty()) {
    Serial.println("No active or completed games found");
    return;
  }
  Serial.print("Time after getting latest game ID: ");
  Serial.println(millis() / 1000.0);
  // 2. Get the last at-bat index
  int lastAtBatIndex = getLastAtBatIndex(gameId);
  
  if(lastAtBatIndex < 0) {
    Serial.println("Could not determine last at-bat");
    return;
  }
  Serial.print("Time after getting latest at-bat #: ");
  Serial.println(millis() / 1000.0);
  // 3. Get pitch data for the last at-bat
  getPitchDataForAtBat(gameId, lastAtBatIndex);
  Serial.print("Time after getting pitch data for said at-bat: ");
  Serial.println(millis() / 1000.0);
}