#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <TimeLib.h>
#include <TwitterWebAPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_PCD8544.h>

// converts Unicode escape sequences to UTF-8 characters for special characters
#define ARDUINOJSON_DECODE_UNICODE 1
#include <ArduinoJson.h>

#include "config.h"

// if AP credentials are not in "config.h", set here:
#ifndef AP_SSID
#define AP_SSID "your-ssid"
#define AP_PASS "your-password"
#endif

// if OTA configuration is not in "config.h", set here:
#ifndef OTA_PASS
#define OTA_PASS "your-password"
#endif
#ifndef OTA_NAME
#define OTA_NAME "ESP8266Twitter"
#endif

// if Twitter Web API configuration is not in "config.h", set here:
#ifndef TWITTER_ACCOUNT
#define TWITTER_ACCOUNT "frigo2naitre"
#endif

std::string search_str = TWITTER_ACCOUNT;

// Define the pins used by the LCD:
const int pinRST = D0, pinCE = D1;
const int pinDC = D2, pinDin = D3, pinCLK = D4;

int screenWidth = 14;
int screenHeight = 7;

int stringStart, stringStop = 0;
int scrollCursor = screenWidth;

// Nokia 5110 LCD initialisation:
Adafruit_PCD8544 lcd =
    Adafruit_PCD8544(pinCLK, pinDin, pinDC, pinCE, pinRST);

const char* ntp_server = "pool.ntp.org";
int timezone = 1;
unsigned long twi_update_interval = 20;
WiFiUDP ntpUDP;
// NTP server pool, offset (in seconds), update interval (in milliseconds)
NTPClient timeClient(ntpUDP, ntp_server, timezone*3600, 60000);

static char const consumer_key[]     = TWITTER_CONSUMER_KEY;
static char const consumer_sec[]     = TWITTER_CONSUMER_SEC;
static char const accesstoken[]      = TWITTER_ACCESS_TOKEN;
static char const accesstoken_sec[]  = TWITTER_ACCESS_TOKEN_SEC;
TwitterClient tcr(timeClient,
    consumer_key, consumer_sec,
    accesstoken, accesstoken_sec);

unsigned long api_mtbs = twi_update_interval * 1000;
unsigned long api_lasttime = 0;
std::string search_msg = "No message yet";
char formattedDateChar[15];
String formattedUser = "";

ESP8266WebServer server(80);


void setup_wifi() {
  // define AP credentials and connect ESP8266 to WiFI
  const char* ssid = AP_SSID;
  const char* password = AP_PASS;
  Serial.printf("Connecting to %s ", ssid);
  lcd.println("Connecting to: ");
  lcd.printf("%s ", ssid);
  lcd.display();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    lcd.display();
  }
  Serial.println();
  lcd.clearDisplay();

  Serial.println("Connected, IP address: ");
  Serial.println(WiFi.localIP());
  lcd.println("Connected,");
  lcd.println("IP address: ");
  lcd.println(WiFi.localIP());
  lcd.display();
  delay(3000);
  lcd.clearDisplay();
}

void setup_ota() {
  // define Arduino OTA settings and initialise
  ArduinoOTA.onStart([](){
    Serial.println("OTA started");
  });
  ArduinoOTA.onEnd([](){
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total){
    Serial.printf("Progress: %u%%\r", (progress/(total/100)));
  });
    ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  
  ArduinoOTA.setHostname(OTA_NAME);
  ArduinoOTA.begin();
  lcd.println("OTA started");
  lcd.display();
  delay(500);
}

void setup_ntp()
{
  // Connect to NTP and force-update time
  tcr.startNTP();
  Serial.println("NTP synced");
  lcd.println("NTP synced");
  lcd.display();
  delay(500);
}

void extractJSON(String tmsg) {
  const char* msg2 = const_cast <char*> (tmsg.c_str());
  // FIXME: random size for json document
  DynamicJsonDocument response(2048*2);
  DeserializationError err = deserializeJson(response, msg2);
  Serial.println("msg2:");
  Serial.println(msg2);

  if (err) {
    Serial.print("Failed to parse JSON: ");
    Serial.println(err.c_str());
    Serial.println(msg2);
    return;
  }
  
  if (response.containsKey("statuses")) {
    String text = response["statuses"][0]["text"];
    if (text != "") {
      search_msg = std::string(text.c_str(), text.length());
    }
    String dateString = response["statuses"][0]["created_at"];
    String user = response["statuses"][0]["user"]["screen_name"];
    formattedUser = "@" + user;

    // date format: "Mon Sep 03 08:08:02 +0000 2012"
    const char *dateFormat = "%a %b %d %T %z %Y";
    // formatted date format: "03 Sep - 08:08"
    const char *formattedDateFormat = "%d %b - %R";

    char dateChar[31];
    dateString.toCharArray(dateChar, 31);

    struct tm date = {0};
    strptime(dateChar, dateFormat, &date);
    strftime(formattedDateChar, sizeof(formattedDateChar),
            formattedDateFormat, &date);
  } else if(response.containsKey("errors")) {
    String err = response["errors"][0];
    search_msg = std::string(err.c_str(), err.length());
  } else {
    Serial.println("No useful data");
  }
  
  response.clear();
  delete [] msg2;
}

void extractTweetText(String tmsg) {
  Serial.print("Received Message Length");
  long msglen = tmsg.length();
  Serial.print(": ");
  Serial.println(msglen);
  if (msglen <= 32) return;
  
  String searchstr = ",\"name\":\""; 
  unsigned int searchlen = searchstr.length();
  int pos1 = -1, pos2 = -1;
  for(long i=0; i <= msglen - searchlen; i++) {
    if(tmsg.substring(i,searchlen+i) == searchstr) {
      pos1 = i + searchlen;
      break;
    }
  }
  searchstr = "\",\""; 
  searchlen = searchstr.length();
  for(long i=pos1; i <= msglen - searchlen; i++) {
    if(tmsg.substring(i,searchlen+i) == searchstr) {
      pos2 = i;
      break;
    }
  }
  String text = tmsg.substring(pos1, pos2);

  searchstr = ",\"followers_count\":"; 
  searchlen = searchstr.length();
  int pos3 = -1, pos4 = -1;
  for(long i=pos2; i <= msglen - searchlen; i++) {
    if(tmsg.substring(i,searchlen+i) == searchstr) {
      pos3 = i + searchlen;
      break;
    }
  }
  searchstr = ","; 
  searchlen = searchstr.length();
  for(long i=pos3; i <= msglen - searchlen; i++) {
    if(tmsg.substring(i,searchlen+i) == searchstr) {
      pos4 = i;
      break;
    }
  }
  String usert = tmsg.substring(pos3, pos4);

  if (text.length() > 0) {
    text =  text + " has " + usert + " followers.";
    search_msg = std::string(text.c_str(), text.length());
  }
}

void handleRoot() {
  String t;
  t += "<html>";
  t += "<head>";
  t += "<meta name='viewport' content='width=device-width,initial-scale=1' />";
  t += "<meta http-equiv='Content-Type' content='text/html; charset=utf-8' />";
  t += "</had>";
  t += "<body>";
  t += "<h3><center>";
  t += "Post to Twitter";
  t += "</center></h3>";
  t += "<center>";
  t += "<form method='POST' action='/tweet'>";
  t += "<input type=text name=text style='width: 40em;' autofocus placeholder='Twitter Message'>";
  t += "<input type=submit name=submit value='Tweet'>";
  t += "</form>";
  t += "</center>";
  t += "</body>";
  t += "</html>";
  server.send(200, "text/html", t);
}

void getSearchWord() {
  String webpage;
  webpage =  "<html>";
  webpage += "<meta name='viewport' content='width=device-width,initial-scale=1' />";
   webpage += "<head><title>Twitter IOT Scrolling Text Display</title>";
    webpage += "<style>";
     webpage += "body { background-color: #E6E6FA; font-family: Arial, Helvetica, Sans-Serif; Color: blue;}";
    webpage += "</style>";
   webpage += "</head>";
   webpage += "<body>";
    webpage += "<br>";  
    webpage += "<form action='/processreadtweet' method='POST'>";
     webpage += "<center><input type='text' name='search_input' value='"+String(search_str.c_str())+"' placeholder='Twitter Search'></center><br>";
     webpage += "<center><input type='submit' value='Update Search Keyword'></center>";
   webpage += "<br><center><a href='/readtweet'>Latest Received Message</a></center>";
    webpage += "</form>";
   webpage += "</body>";
  webpage += "</html>";
  server.send(200, "text/html", webpage); // Send a response to the client asking for input
}

void handleTweet() {
  if (server.method() == HTTP_POST) {
    std::string text;
    bool submit = false;
    for (uint8_t i=0; i<server.args(); i++){
      if (server.argName(i) == "text") {
        String s = server.arg(i);
        text = std::string(s.c_str(), s.length());
      } else if (server.argName(i) == "submit") {
        submit = true;
      }
    }
   if (submit && !text.empty()) tcr.tweet(text);
   server.sendHeader("Location", "/", true);
   server.send(302, "text/plain", "");
  }
}

void readTweet(){
  String t;
  t += "<html>";
  t += "<head>";
  t += "<meta name='viewport' content='width=device-width,initial-scale=1' />";
  t += "<meta http-equiv='Content-Type' content='text/html; charset=utf-8' />";
  t += "</had>";
  t += "<body>";
  t += "<center><p>Searching Twitter for: " + String(search_str.c_str()) + "</p></center>";
  t += "<center><p>Latest Message: " + String(search_msg.c_str()) + "</p></center>";
  t += "<br><center><a href='/search'>Update Search Term?</a></center>";
  t += "</form>";
  t += "</body>";
  t += "</html>";
  server.send(200, "text/html", t);
}

void processReadTweet(){
  if (server.args() > 0 and server.method() == HTTP_POST) { // Arguments were received
    for ( uint8_t i = 0; i < server.args(); i++ ) {
      Serial.print(server.argName(i)); // Display the argument
      if (server.argName(i) == "search_input") {
        search_str=std::string(server.arg(i).c_str());
      }
    }
  }
  
  String t;
  t += "<html>";
  t += "<head>";
  t += "<meta name='viewport' content='width=device-width,initial-scale=1' />";
  t += "<meta http-equiv='Content-Type' content='text/html; charset=utf-8' />";
  t += "</had>";
  t += "<body>";
  t += "<center><p>Updated search term: " + String(search_str.c_str()) + "</p></center>";
  t += "<br><center><a href='http://" + WiFi.localIP().toString() + "/search'>Update again?</a></center>";
  t += "</form>";
  t += "</body>";
  t += "</html>";
  server.send(200, "text/html", t);
}

void handleNotFound(){
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

// Adafruit GFX does not support international characters
// FIXME: crappy workaround...
void replaceAccentedChars(String *str)
{
   str->replace("à", "a");
   str->replace("é", "e");
   str->replace("è", "e");
   str->replace("&lt;", "<");
}

void setup()
{
  if (twi_update_interval < 5) api_mtbs = 5000; // Cant update faster than 5s.
  
  // Connect ESP8266 to local Wifi
  Serial.begin(115200);
  Serial.println();

  // Initialise and configure LCD
  lcd.begin();
  lcd.setContrast(55);
  lcd.setTextSize(1);
  delay(2);
  lcd.clearDisplay();

  setup_wifi();
  setup_ota();
  setup_ntp();

  // Setup internal LED
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  server.on("/", handleRoot);
  server.on("/search", getSearchWord);
  server.on("/tweet", handleTweet);
  server.on("/readtweet",readTweet);
  server.on("/processreadtweet",processReadTweet);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("HTTP server started");
  lcd.println("HTTP started");
  lcd.display();
  delay(500);

  lcd.clearDisplay();
}

void loop()
{
  ArduinoOTA.handle();
  server.handleClient();

  if (millis() > api_lasttime + api_mtbs)  {
    digitalWrite(LED_BUILTIN, LOW);
    //extractJSON(tcr.mentionsTimeline());
    extractJSON(tcr.userTimeline(search_str));
    //extractJSON(tcr.searchTwitter(search_str));
    //extractTweetText(tcr.searchUser(search_str));
    Serial.print("Search: ");
    Serial.println(search_str.c_str());
    Serial.print("MSG: ");
    Serial.println(search_msg.c_str());
    api_lasttime = millis();
    twi_update_interval = 180;
    api_mtbs = twi_update_interval * 1000;
  }
  delay(2);
  yield();
  digitalWrite(LED_BUILTIN, HIGH);

  String msg = search_msg.c_str();
  if (formattedUser == "")
  {
    lcd.setCursor(0, 0);
    lcd.println(msg);
    delay(500);
    Serial.print(".");
    lcd.println("...");
    lcd.display();
  }
  else
  {
    lcd.setCursor(0, 0);
    lcd.setTextColor(WHITE, BLACK);
    lcd.println(formattedDateChar);
    lcd.setTextColor(BLACK);
    lcd.println(formattedUser);
    lcd.println();

    String formattedMsg = msg.substring(stringStart, stringStop);
    replaceAccentedChars(&formattedMsg);
    lcd.print(formattedMsg);
    lcd.display();
    delay(250);
    lcd.clearDisplay();
      if(stringStart == 0 && scrollCursor > 0){
      scrollCursor--;
      stringStop++;
    } else if (stringStart == stringStop){
      stringStart = stringStop = 0;
      scrollCursor = screenWidth;
    } else if (stringStop == msg.length() && scrollCursor == 0) {
      stringStart++;
    } else {
      stringStart++;
      stringStop++;
    }
  }
}

