#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// -- USER CONFIG ----------------------------------------------
const char* WIFI_SSID         = "PISO WIFI";
const char* WIFI_PASSWORD     = "singko5minutes";
const char* SUPABASE_URL      = "https://crottwcwsasedjzvydbp.supabase.co";
const char* SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImNyb3R0d2N3c2FzZWRqenZ5ZGJwIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzkyODcxMzIsImV4cCI6MjA5NDg2MzEzMn0.qQU8eT4pkSIBILkLXy2gkE_4RDpybTxrfRjYD72MiGs";
const char* DEVICE_ID         = "esp32-troyikz-01";
const char* FIRMWARE_VER      = "v1.2.0";

const long  GMT_OFFSET_SEC    = 28800;
const int   DAYLIGHT_OFFSET   = 0;
const char* NTP_SERVER        = "pool.ntp.org";

#define PIN_MQ_SENSOR  34
#define PIN_LED_GREEN  26
#define PIN_LED_RED    27
#define PIN_BUZZER     25

const unsigned long WARMUP_MS          = 180000UL;
const unsigned long READ_INTERVAL_MS   = 50UL;
const unsigned long UPLOAD_INTERVAL_MS = 2000UL;
const unsigned long HEARTBEAT_MS       = 3000UL;
const unsigned long WIFI_RETRY_MS      = 10000UL;
const unsigned long BLINK_MS           = 50UL;
const unsigned long COMMAND_POLL_MS    = 1000UL;

const int NORMAL_THRESHOLD  = 320;
const int HAZARD_THRESHOLD  = 1000;
const int ADC_SAMPLES       = 5;

enum SmokeClass { CLASS_AIR=0, CLASS_NORMAL=1, CLASS_HAZARDOUS=2 };
const char* CLASS_LABELS[] = { "AIR", "NORMAL SMOKE", "HAZARDOUS SMOKE" };

String EP_LOGS, EP_STATUS, EP_COMMANDS;

bool          sensorReady    = false;
bool          isTesting      = false;
bool          wifiConnected  = false;
int           testCounter    = 0;
SmokeClass    prevClass      = CLASS_AIR;
String        currentSession = "";
bool          overrideActive = false;
SmokeClass    overrideClass  = CLASS_AIR;
unsigned long lastBlinkTime  = 0;
bool          blinkState     = false;
unsigned long warmupStart    = 0;
unsigned long lastReadTime   = 0;
unsigned long lastUploadTime = 0;
unsigned long lastHeartbeat  = 0;
unsigned long lastWifiRetry  = 0;
unsigned long lastCmdPoll    = 0;

void connectWifi();
void syncTime();
int  readSmokeSensor();
SmokeClass classifySmoke(int v);
void actuate(SmokeClass cls, unsigned long now);
void printClassAlert(SmokeClass cls, int val);
void applyCommand(const char* cmd);
void pollCommands();
void markCommandDone(long id);
void sendLogToSupabase(int val, SmokeClass cls, const String& sid, const String& ss);
void sendHeartbeat();
void setLEDs(bool g, bool r, bool b);
void startSession();
void stopSession();
void handleSerial();
String buildTestId(int n);
String isoNow();
const char* classStatus(SmokeClass c);

// -- SETUP ----------------------------------------------------
void setup() {
  Serial.begin(115200); delay(150);
  Serial.println(F("\n============================================================"));
  Serial.print(F("  TROYIKZ  ")); Serial.println(FIRMWARE_VER);
  Serial.println(F("  AIR(<320) | NORMAL(320-999) | HAZARDOUS(1000+)"));
  Serial.println(F("  S=Start X=Stop  0=Air 1=Normal 2=Hazardous A=Auto"));
  Serial.println(F("============================================================\n"));
  pinMode(PIN_LED_GREEN,OUTPUT); pinMode(PIN_LED_RED,OUTPUT); pinMode(PIN_BUZZER,OUTPUT);
  analogReadResolution(10); analogSetAttenuation(ADC_11db);
  setLEDs(false,false,false);
  EP_LOGS     = String(SUPABASE_URL)+"/rest/v1/smoke_logs";
  EP_STATUS   = String(SUPABASE_URL)+"/rest/v1/sensor_status";
  EP_COMMANDS = String(SUPABASE_URL)+"/rest/v1/commands";
  for(int i=0;i<3;i++){digitalWrite(PIN_LED_GREEN,HIGH);delay(100);digitalWrite(PIN_LED_GREEN,LOW);delay(100);}
  connectWifi(); syncTime();
  warmupStart=millis();
  Serial.println(F("[WARMUP] Started - 3 minutes.\n"));
  sendHeartbeat();
}

// -- LOOP -----------------------------------------------------
void loop() {
  unsigned long now=millis();
  if(!wifiConnected&&now-lastWifiRetry>=WIFI_RETRY_MS){lastWifiRetry=now;connectWifi();}
  handleSerial();

  // Poll commands always when WiFi is up (not just after warmup)
  // This allows S/X session commands to work from dashboard during warmup
  if(wifiConnected&&now-lastCmdPoll>=COMMAND_POLL_MS){lastCmdPoll=now;pollCommands();}

  if(!sensorReady){
    unsigned long el=now-warmupStart;
    if(el>=WARMUP_MS){
      sensorReady=true; setLEDs(true,false,false);
      Serial.println(F("\n[WARMUP] Complete! Sensor ready."));
      sendHeartbeat(); delay(800); setLEDs(false,false,false);
    } else {
      bool tick=(now/800)%2==0;
      digitalWrite(PIN_LED_GREEN,tick?HIGH:LOW);
      digitalWrite(PIN_LED_RED,!tick?HIGH:LOW);
    }
  }

  if(now-lastHeartbeat>=HEARTBEAT_MS){
    lastHeartbeat=now; sendHeartbeat();
    if(!sensorReady){unsigned long r=(WARMUP_MS-(now-warmupStart))/1000;Serial.printf("[WARMUP] %02lu:%02lu\n",r/60,r%60);}
  }

  if(isTesting&&now-lastReadTime>=READ_INTERVAL_MS){
    lastReadTime=now;
    int val=readSmokeSensor();
    SmokeClass cls=overrideActive?overrideClass:classifySmoke(val);
    if(cls!=prevClass){printClassAlert(cls,val);}
    prevClass=cls;
    float v=val*3.3f/1023.0f;
    Serial.printf("[%s] AO:%4d | %.3fV | %-15s | %s%s\n",
      currentSession.c_str(),val,v,CLASS_LABELS[(int)cls],
      overrideActive?"[OVR] ":"",classStatus(cls));
    if(now-lastUploadTime>=UPLOAD_INTERVAL_MS){lastUploadTime=now;sendLogToSupabase(val,cls,currentSession,"active");}
  }

  // Run actuate EVERY loop tick using prevClass — this makes blink
  // independent of HTTP blocking delays. BLINK_MS controls the speed.
  if(isTesting) actuate(prevClass, millis());
  if(sensorReady&&!isTesting) setLEDs(false,false,false);
}

// -- WIFI -----------------------------------------------------
void connectWifi(){
  if(WiFi.status()==WL_CONNECTED){wifiConnected=true;return;}
  Serial.printf("[WiFi] Connecting to %s",WIFI_SSID);
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  unsigned long s=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-s<10000UL){delay(500);Serial.print('.');}
  if(WiFi.status()==WL_CONNECTED){wifiConnected=true;Serial.printf("\n[WiFi] OK  IP=%s  RSSI=%d\n",WiFi.localIP().toString().c_str(),(int)WiFi.RSSI());}
  else{wifiConnected=false;Serial.println(F("\n[WiFi] Failed - retrying."));}
}

// -- NTP ------------------------------------------------------
void syncTime(){
  if(!wifiConnected){Serial.println(F("[NTP] Skipped."));return;}
  configTime(GMT_OFFSET_SEC,DAYLIGHT_OFFSET,NTP_SERVER);
  Serial.print(F("[NTP] Syncing"));
  struct tm t; unsigned long s=millis();
  while(!getLocalTime(&t)&&millis()-s<8000UL){delay(500);Serial.print('.');}
  if(getLocalTime(&t)){char b[32];strftime(b,sizeof(b),"%Y-%m-%d %H:%M:%S",&t);Serial.printf("\n[NTP] %s\n",b);}
  else Serial.println(F("\n[NTP] Failed."));
}

// -- POLL DASHBOARD COMMANDS ----------------------------------
void pollCommands(){
  HTTPClient http;
  http.begin(EP_COMMANDS+"?executed=eq.false&order=created_at.asc&limit=1");
  http.setTimeout(2000);
  http.addHeader("apikey",SUPABASE_ANON_KEY);
  http.addHeader("Authorization",String("Bearer ")+SUPABASE_ANON_KEY);
  int code=http.GET();
  if(code==200){
    String body=http.getString();
    StaticJsonDocument<256> doc;
    if(!deserializeJson(doc,body)&&doc.is<JsonArray>()&&doc.as<JsonArray>().size()>0){
      JsonObject cmd=doc[0];
      long id=cmd["id"].as<long>();
      const char* command=cmd["command"].as<const char*>();
      Serial.printf("[CMD] Dashboard command: '%s' id=%ld\n",command,id);
      applyCommand(command);
      markCommandDone(id);
    }
  }
  http.end();
}

void markCommandDone(long id){
  HTTPClient http;
  http.begin(EP_COMMANDS+"?id=eq."+String(id));
  http.setTimeout(2000);
  http.addHeader("Content-Type","application/json");
  http.addHeader("apikey",SUPABASE_ANON_KEY);
  http.addHeader("Authorization",String("Bearer ")+SUPABASE_ANON_KEY);
  http.addHeader("Prefer","return=minimal");
  int code=http.PATCH("{\"executed\":true}");
  if(code==204||code==200) Serial.printf("[CMD] Done id=%ld\n",id);
  else Serial.printf("[CMD] PATCH failed HTTP=%d\n",code);
  http.end();
}

// -- APPLY COMMAND (serial + dashboard) -----------------------
// Handles: 0/1/2/A = override tiers, S = start session, X = stop session
void applyCommand(const char* cmd){
  if(strcmp(cmd,"0")==0){
    overrideActive=true; overrideClass=CLASS_AIR; prevClass=CLASS_HAZARDOUS;
    Serial.println(F("[CMD] -> Force AIR"));
  }
  else if(strcmp(cmd,"1")==0){
    overrideActive=true; overrideClass=CLASS_NORMAL; prevClass=CLASS_AIR;
    Serial.println(F("[CMD] -> Force NORMAL SMOKE"));
  }
  else if(strcmp(cmd,"2")==0){
    overrideActive=true; overrideClass=CLASS_HAZARDOUS; prevClass=CLASS_NORMAL;
    Serial.println(F("[CMD] -> Force HAZARDOUS"));
  }
  else if(cmd[0]=='A'||cmd[0]=='a'){
    overrideActive=false; prevClass=CLASS_AIR;
    Serial.println(F("[CMD] -> AUTO (sensor-driven)"));
  }
  else if(cmd[0]=='S'||cmd[0]=='s'){
    Serial.println(F("[CMD] -> Start Session"));
    startSession();
  }
  else if(cmd[0]=='X'||cmd[0]=='x'){
    Serial.println(F("[CMD] -> Stop Session"));
    stopSession();
  }
}

// -- CLASSIFY -------------------------------------------------
SmokeClass classifySmoke(int v){
  if(v>=HAZARD_THRESHOLD) return CLASS_HAZARDOUS;
  if(v>=NORMAL_THRESHOLD) return CLASS_NORMAL;
  return CLASS_AIR;
}

// -- ACTUATE --------------------------------------------------
void actuate(SmokeClass cls,unsigned long now){
  switch(cls){
    case CLASS_AIR:
      digitalWrite(PIN_LED_GREEN,HIGH);digitalWrite(PIN_LED_RED,LOW);digitalWrite(PIN_BUZZER,LOW);
      break;
    case CLASS_NORMAL:
      if(now-lastBlinkTime>=BLINK_MS){lastBlinkTime=now;blinkState=!blinkState;digitalWrite(PIN_LED_GREEN,blinkState?HIGH:LOW);}
      digitalWrite(PIN_LED_RED,LOW);digitalWrite(PIN_BUZZER,LOW);
      break;
    case CLASS_HAZARDOUS:
      digitalWrite(PIN_LED_GREEN,LOW);digitalWrite(PIN_LED_RED,HIGH);digitalWrite(PIN_BUZZER,HIGH);
      break;
  }
}
void setLEDs(bool g,bool r,bool b){
  digitalWrite(PIN_LED_GREEN,g?HIGH:LOW);
  digitalWrite(PIN_LED_RED,r?HIGH:LOW);
  digitalWrite(PIN_BUZZER,b?HIGH:LOW);
}

// -- TRANSITION BANNERS ---------------------------------------
void printClassAlert(SmokeClass cls,int val){
  float v=val*3.3f/1023.0f;
  switch(cls){
    case CLASS_AIR:       Serial.printf("\n=== AIR - CLEAR  AO:%d %.3fV ===\n",val,v);break;
    case CLASS_NORMAL:    Serial.printf("\n~~~ NORMAL SMOKE  AO:%d %.3fV ~~~\n",val,v);break;
    case CLASS_HAZARDOUS: Serial.printf("\n!!! HAZARDOUS  AO:%d %.3fV  RED+BUZZER !!!\n",val,v);break;
  }
}

// -- SENSOR READ ----------------------------------------------
int readSmokeSensor(){
  long s=0;
  for(int i=0;i<ADC_SAMPLES;i++){s+=analogRead(PIN_MQ_SENSOR);delay(2);}
  return(int)(s/ADC_SAMPLES);
}

// -- SESSION CONTROL ------------------------------------------
void startSession(){
  if(isTesting){Serial.println(F("[SESSION] Already running."));return;}
  if(!sensorReady){
    unsigned long r=(WARMUP_MS-(millis()-warmupStart))/1000;
    Serial.printf("[SESSION] Cannot start - warming up (%lu sec left).\n",r);
    return;
  }
  isTesting=true; lastReadTime=0; lastUploadTime=0; prevClass=CLASS_AIR;
  currentSession=buildTestId(testCounter+1);
  Serial.printf("\n[SESSION] Started %s  NORMAL>=%d  HAZ>=%d\n",
    currentSession.c_str(),NORMAL_THRESHOLD,HAZARD_THRESHOLD);
}

void stopSession(){
  if(!isTesting){Serial.println(F("[SESSION] Nothing to stop."));return;}
  isTesting=false; testCounter++;
  int val=readSmokeSensor();
  SmokeClass cls=overrideActive?overrideClass:classifySmoke(val);
  Serial.printf("[SESSION] Stopped %s  AO=%d  %s\n",
    currentSession.c_str(),val,CLASS_LABELS[(int)cls]);
  sendLogToSupabase(val,cls,currentSession,"completed");
  delay(1500); setLEDs(false,false,false); currentSession=""; prevClass=CLASS_AIR;
}

// -- SUPABASE LOG ---------------------------------------------
void sendLogToSupabase(int val,SmokeClass cls,const String& sid,const String& ss){
  if(!wifiConnected){Serial.printf("[DB] No WiFi (%s)\n",ss.c_str());return;}
  HTTPClient http; http.begin(EP_LOGS);
  http.setTimeout(2000);
  http.addHeader("Content-Type","application/json");
  http.addHeader("apikey",SUPABASE_ANON_KEY);
  http.addHeader("Authorization",String("Bearer ")+SUPABASE_ANON_KEY);
  http.addHeader("Prefer","return=minimal");
  StaticJsonDocument<320> doc;
  doc["test_id"]=sid;
  doc["smoke_value"]=val;
  doc["classification"]=CLASS_LABELS[(int)cls];
  doc["status"]=classStatus(cls);
  doc["green_led"]=(cls==CLASS_AIR||cls==CLASS_NORMAL);
  doc["red_led"]=(cls==CLASS_HAZARDOUS);
  doc["buzzer"]=(cls==CLASS_HAZARDOUS);
  doc["override"]=overrideActive;
  doc["session_status"]=ss;
  doc["created_at"]=isoNow();
  String body; serializeJson(doc,body);
  int code=http.POST(body);
  if(code==201) Serial.printf("[DB] OK %s AO=%d %s [%s]\n",sid.c_str(),val,CLASS_LABELS[(int)cls],ss.c_str());
  else Serial.printf("[DB] Failed HTTP=%d\n",code);
  http.end();
}

// -- HEARTBEAT ------------------------------------------------
// Sends is_online=true while running; dashboard detects staleness by last_seen age
void sendHeartbeat(){
  if(!wifiConnected) return;
  unsigned long now=millis(),el=now-warmupStart;
  int rem=sensorReady?0:(int)max(0L,(long)((WARMUP_MS-el)/1000));
  HTTPClient http; http.begin(EP_STATUS+"?on_conflict=device_id");
  http.setTimeout(2000);
  http.addHeader("Content-Type","application/json");
  http.addHeader("apikey",SUPABASE_ANON_KEY);
  http.addHeader("Authorization",String("Bearer ")+SUPABASE_ANON_KEY);
  http.addHeader("Prefer","resolution=merge-duplicates,return=minimal");
  StaticJsonDocument<256> doc;
  doc["device_id"]=DEVICE_ID;
  doc["is_online"]=true;
  doc["is_initialized"]=sensorReady;
  doc["warmup_remaining"]=rem;
  doc["rssi"]=(int)WiFi.RSSI();
  doc["ip_address"]=WiFi.localIP().toString();
  doc["firmware_ver"]=FIRMWARE_VER;
  doc["last_seen"]=isoNow();
  String body; serializeJson(doc,body);
  int code=http.POST(body);
  if(code!=200&&code!=201&&code!=204) Serial.printf("[HB] Failed HTTP=%d\n",code);
  http.end();
}

// -- SERIAL ---------------------------------------------------
void handleSerial(){
  if(!Serial.available()) return;
  char c=Serial.read();
  switch(c){
    case 'S':case 's': startSession(); break;
    case 'X':case 'x': stopSession();  break;
    case '0':case '1':case '2':{char cmd[2]={c,'\0'};applyCommand(cmd);break;}
    case 'A':case 'a': applyCommand("A"); break;
  }
}

// -- HELPERS --------------------------------------------------
String buildTestId(int n){
  char b[12]; snprintf(b,sizeof(b),"TEST-%04d",n); return String(b);
}
String isoNow(){
  struct tm t;
  if(!getLocalTime(&t)){
    unsigned long s=millis()/1000;
    char b[32];
    snprintf(b,sizeof(b),"1970-01-01T%02lu:%02lu:%02luZ",(s/3600)%24,(s/60)%60,s%60);
    return String(b);
  }
  char b[32]; strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%S+08:00",&t); return String(b);
}
const char* classStatus(SmokeClass c){
  switch(c){
    case CLASS_AIR:       return "Safe";
    case CLASS_NORMAL:    return "Normal";
    case CLASS_HAZARDOUS: return "Hazardous";
  }
  return "Unknown";
}

/*  WIRING
    GPIO34 -> MQ AOUT  |  GPIO26 -> Green LED  |  GPIO27 -> Red LED  |  GPIO25 -> Buzzer  */
