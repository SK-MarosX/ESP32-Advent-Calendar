#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <time.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

// Declare this type before Arduino IDE auto-generates function prototypes.
struct ClockInfo{tm t;bool valid;};

// ESP32-C3 SuperMini: NeoPixel DIN GPIO3, onboard status LED GPIO8 (board-dependent).
// Wi-Fi: viac ulozenych sieti + sucasne konfig. AP a klient (STA).
constexpr uint8_t MAX_WIFI_NETWORKS=5;
const char* CONFIG_AP_SSID="Advent-Setup";
const char* CONFIG_AP_PASSWORD="advent123"; // zmente, ak chcete vlastne heslo AP
struct SavedWiFi { String ssid; String password; };
SavedWiFi savedWiFi[MAX_WIFI_NETWORKS];
uint8_t savedWiFiCount=0;
String connectedSsid="";
uint32_t wifiScanRetry=0;
bool timeSyncConfigured=false;
volatile bool wifiConfigChanged=false;
constexpr uint8_t LED_PIN=3, ONBOARD_LED_PIN=8;
constexpr bool ONBOARD_LED_ACTIVE_LOW=true;
constexpr int N=24;                 // Logicke adventne LED
constexpr int PHYSICAL_LED_COUNT=101; // Fyzicka dlzka celeho LED pasu
constexpr uint8_t DEFAULT_BRIGHTNESS=80, OPENED_BRIGHTNESS=28;
Adafruit_NeoPixel strip(PHYSICAL_LED_COUNT,LED_PIN,NEO_GRB+NEO_KHZ800);
WebServer server(80); Preferences prefs;
uint8_t brightness=DEFAULT_BRIGHTNESS;
uint32_t prodMask=0,testMask=0;
int prodSeason=-1, prodPlan[N], testPlan[N], prodActive=-1, testActive=-1;
bool testMode=false, manualMode=false, effectMode=false;
int manualLed=-1,effectType=0,manualEffect=0; bool rouletteMode=false, rouletteResultMode=false; uint32_t rouletteStart=0,rouletteDuration=6500,rouletteNext=0,rouletteResultStart=0; int rouletteLed=0; uint8_t rouletteStyle=1; uint32_t rouletteInterval=45;
// Daily active-window effect: mode 0=fixed, 1=random per date, 2=cycle.
uint8_t dailyEffectMode=0, dailyEffect=0, dailyEffectSpeed=50;
uint8_t testEffectSpeed=50;
int dailyEffectDate=-1, dailyEffectChosen=0;
time_t testEpoch=0;
int lastProdDate=-1,lastTestDate=-1;
uint32_t lastAnim=0,lastPrint=0,lastStatus=0,lastWifiRetry=0;
bool mdnsStarted=false;
volatile bool pendingTestUpdate=false;

void clearPlan(int *p){for(int i=0;i<N;i++)p[i]=-1;}
ClockInfo clockNow(){ClockInfo c{};time_t n;if(testMode)n=testEpoch;else time(&n);if(n<1700000000){c.valid=false;return c;}localtime_r(&n,&c.t);c.valid=true;return c;}
int keyDate(const tm&t){return (t.tm_year+1900)*10000+(t.tm_mon+1)*100+t.tm_mday;}
int phaseOf(const tm&t){int m=t.tm_mon+1,d=t.tm_mday;if(m==12&&d<=24)return 1;if((m==12&&d>=25)||(m==1&&d<=6))return 2;return 0;}
int bits(uint32_t m){return __builtin_popcount(m&0xffffffUL);}
int chooseLed(uint32_t mask){uint8_t a[N];int n=0;for(int i=0;i<N;i++)if(!(mask&(1UL<<i)))a[n++]=i;return n?a[random(n)]:-1;}
void saveProd(){prefs.putUInt("mask",prodMask);prefs.putInt("season",prodSeason);prefs.putBytes("plan",prodPlan,sizeof(prodPlan));prefs.putInt("active",prodActive);}
void loadProd(){prodMask=prefs.getUInt("mask",0)&0xffffffUL;prodSeason=prefs.getInt("season",-1);clearPlan(prodPlan);if(prefs.getBytesLength("plan")==sizeof(prodPlan))prefs.getBytes("plan",prodPlan,sizeof(prodPlan));prodActive=prefs.getInt("active",-1);for(int i=0;i<N;i++)if(prodPlan[i]<-1||prodPlan[i]>=N)prodPlan[i]=-1;}
void resetTest(){testMask=0;clearPlan(testPlan);testActive=-1;lastTestDate=-1;}
void resetProd(int y){prodMask=0;clearPlan(prodPlan);prodActive=-1;prodSeason=y;saveProd();}
int dailyDateKey(const tm&t){return keyDate(t);}
int effectForDate(const tm&t){
 int key=dailyDateKey(t);
 if(dailyEffectMode==0)return dailyEffect;
 if(dailyEffectMode==1){
  // Stable pseudo-random selection for this calendar date; unchanged by refresh/reboot.
  uint32_t x=(uint32_t)key*2654435761UL+0x9e3779b9UL;
  x^=x>>16; x*=0x7feb352dUL; x^=x>>15;
  return (int)(x%9);
 }
 return (key%9);
}
uint32_t scaleColor(uint32_t color,float factor){
 uint8_t r=(color>>16)&255,g=(color>>8)&255,b=color&255;
 return strip.Color((uint8_t)constrain((int)(r*factor),0,255),
                    (uint8_t)constrain((int)(g*factor),0,255),
                    (uint8_t)constrain((int)(b*factor),0,255));
}
uint32_t dailyColor(int fx,uint32_t ms,int led){
 float speed=dailyEffectSpeed/50.0f;
 float phase=(ms*speed/1000.0f)+(led*0.73f);
 float wave=(1.0f-cosf(phase*2.0f*PI))*0.5f;
 uint8_t v=0,r=0,g=0,b=0;
 switch(fx){
  case 0: // Warm breathing
   v=constrain((int)(20+150*wave),0,255); r=v;g=(uint8_t)(v*.62f);b=(uint8_t)(v*.22f);break;
  case 1: // Candle flame
   v=random(0,100)<8?random(55,150):constrain((int)(65+80*wave+random(-14,15)),20,170);
   r=v;g=(uint8_t)(v*.42f);b=(uint8_t)(v*.07f);break;
  case 2: // Soft flicker
   v=constrain((int)(75+random(-18,19)),35,120);r=v;g=(uint8_t)(v*.78f);b=(uint8_t)(v*.45f);break;
  case 3: // Twinkle
   v=((ms+(uint32_t)(led*191))%(uint32_t)max(450,1500-(int)(dailyEffectSpeed*8)))<120?170:24;
   r=v;g=(uint8_t)(v*.75f);b=(uint8_t)(v*.42f);break;
  case 4: // RGB rainbow cycle
   {float hue=fmodf(ms*speed/24.0f+led*18.0f,360.0f);float c=1.0f,x=1.0f-fabsf(fmodf(hue/60.0f,2.0f)-1.0f);
    float rr=0,gg=0,bb=0;if(hue<60){rr=c;gg=x;}else if(hue<120){rr=x;gg=c;}else if(hue<180){gg=c;bb=x;}else if(hue<240){gg=x;bb=c;}else if(hue<300){rr=x;bb=c;}else{rr=c;bb=x;}
    r=(uint8_t)(rr*155);g=(uint8_t)(gg*155);b=(uint8_t)(bb*155);}
   break;
  case 5: // Aurora teal/purple
   r=(uint8_t)(18+40*wave);g=(uint8_t)(55+100*wave);b=(uint8_t)(90+120*(1-wave));break;
  case 6: // Christmas red/green alternating pulse
   if(((ms/(uint32_t)max(180,900-(int)(dailyEffectSpeed*7))+led/2)%2)==0){r=(uint8_t)(100+100*wave);g=8;b=4;}
   else{r=4;g=(uint8_t)(85+100*wave);b=12;}break;
  case 7: // Ice blue/white sparkle
   v=(uint8_t)(35+90*wave);r=(uint8_t)(v*.45f);g=(uint8_t)(v*.75f);b=v;
   if(((ms+led*233)%1800)<95){r=160;g=210;b=255;}break;
  case 8: // Steady amber glow
   r=150;g=75;b=18;break;
  default:r=120;g=70;b=20;break;
 }
 return strip.Color(r,g,b);
}
void loadDailyEffect(){dailyEffectMode=prefs.getUChar("fxMode",0);dailyEffect=prefs.getUChar("fxDaily",0);dailyEffectSpeed=prefs.getUChar("fxSpeed",50);if(dailyEffectMode>2)dailyEffectMode=0;if(dailyEffect>8)dailyEffect=0;if(dailyEffectSpeed<10||dailyEffectSpeed>100)dailyEffectSpeed=50;}
void saveDailyEffect(){prefs.putUChar("fxMode",dailyEffectMode);prefs.putUChar("fxDaily",dailyEffect);prefs.putUChar("fxSpeed",dailyEffectSpeed);}
void processProd(const tm&t){int k=keyDate(t);if(k==lastProdDate)return;lastProdDate=k;int y=t.tm_year+1900,m=t.tm_mon+1,d=t.tm_mday;if(m==12&&d==1&&prodSeason!=y)resetProd(y);else if(prodSeason<0&&m==12){prodSeason=y;saveProd();}
 if(m==12&&d>=1&&d<=24){for(int day=1;day<=d;day++)if(prodPlan[day-1]<0){int x=chooseLed(prodMask);if(x<0)break;prodPlan[day-1]=x;prodMask|=1UL<<x;}prodActive=prodPlan[d-1];saveProd();}else prodActive=-1;
 }
void processTest(const tm&t){int k=keyDate(t);if(k==lastTestDate)return;lastTestDate=k;int m=t.tm_mon+1,d=t.tm_mday;if(m==12&&d>=1&&d<=24){for(int day=1;day<=d;day++)if(testPlan[day-1]<0){int x=chooseLed(testMask);if(x<0)break;testPlan[day-1]=x;testMask|=1UL<<x;}testActive=testPlan[d-1];}else testActive=-1;}
bool parseDT(String s,tm&o){int y,m,d,h,mi,se;if(sscanf(s.c_str(),"%d-%d-%d %d:%d:%d",&y,&m,&d,&h,&mi,&se)!=6)return false;if(y<2020||y>2099||m<1||m>12||d<1||d>31||h<0||h>23||mi<0||mi>59||se<0||se>59)return false;memset(&o,0,sizeof(o));o.tm_year=y-1900;o.tm_mon=m-1;o.tm_mday=d;o.tm_hour=h;o.tm_min=mi;o.tm_sec=se;o.tm_isdst=-1;time_t e=mktime(&o);tm v{};localtime_r(&e,&v);return v.tm_year==y-1900&&v.tm_mon==m-1&&v.tm_mday==d;}
String phaseName(int p){return p==1?"ADVENT":p==2?"TWINKLE":"OFF";}
String symbols(uint32_t ms){ClockInfo c=clockNow();int p=c.valid?phaseOf(c.t):0;uint32_t mask=testMode?testMask:prodMask;int active=manualMode?manualLed:testMode?testActive:prodActive;float w=(1-cosf((ms%2800)/2800.f*2*PI))*.5f;String s="ESP";for(int i=0;i<N;i++){char ch='.';if(manualMode)ch=i==manualLed?'*':'.';else if(effectMode){uint32_t q=(ms/350+i*7)%17;ch=effectType==1?(i==(ms/800)%N?'*':'.'):(q<3?'.':q<8?':':q<13?'+':'*');}else if(p==1&&(mask&(1UL<<i)))ch=i==active?(w>.48?'*':'+'):'o';else if(p==2){uint32_t q=(ms/350+i*7)%17;ch=q<3?'.':q<8?':':q<13?'+':'*';}s+='-';s+=ch;}return s;}
void ledStatus(bool valid){int n=WiFi.status()!=WL_CONNECTED?2:!valid?3:1;uint32_t t=millis()%2000;bool on=false;for(int i=0;i<n;i++)if(t>=i*260&&t<i*260+110)on=true;digitalWrite(ONBOARD_LED_PIN,(on^ONBOARD_LED_ACTIVE_LOW)?HIGH:LOW);}
uint32_t testEffectColor(int fx,uint32_t ms,int i){
 ms=(uint32_t)((uint64_t)ms*testEffectSpeed/50); // 10% = pomaly, 50% = normalne, 100% = rychlo
 float w=(1.0f-cosf((ms%2400)/2400.0f*2*PI))*0.5f;
 uint8_t v;
 switch(fx){
  case 1: v=18+(uint8_t)(140*w); return strip.Color(v,(uint8_t)(v*.72f),(uint8_t)(v*.4f));
  case 2: v=(uint8_t)(random(12,80)); return strip.Color(v,(uint8_t)(v*.75f),(uint8_t)(v*.45f));
  case 3: {uint8_t r,g,b;uint8_t pos=(ms/20+i*11)%255;if(pos<85){r=pos*3;g=255-pos*3;b=0;}else if(pos<170){pos-=85;r=255-pos*3;g=0;b=pos*3;}else{pos-=170;r=0;g=pos*3;b=255-pos*3;}return strip.Color(r,g,b);}
  case 4: return strip.Color(25+(uint8_t)(30*w),55+(uint8_t)(90*w),100+(uint8_t)(130*(1-w)));
  case 5: v=random(40,180); return strip.Color(v,(uint8_t)(v*.35f),(uint8_t)(v*.04f));
  case 6: return ((ms/600+i)%2)?strip.Color(5,130,12):strip.Color(160,4,2);
  case 7: v=(uint8_t)(35+100*w); return strip.Color((uint8_t)(v*.45f),(uint8_t)(v*.8f),v);
  case 8: {uint8_t r,g,b;uint8_t pos=(ms/12+i*17)%255;if(pos<85){r=pos*3;g=255-pos*3;b=30;}else if(pos<170){pos-=85;r=255-pos*3;g=30;b=pos*3;}else{pos-=170;r=30;g=pos*3;b=255-pos*3;}return strip.Color(r,g,b);}
  case 9: {int head=(ms/45)%N;int d=(head-i+N)%N;uint8_t v=d==0?255:d==1?145:d==2?70:d==3?25:0;return strip.Color(v,(uint8_t)(v*.72f),(uint8_t)(v*.35f));}
  case 10:{int head=(ms/38)%N;int d=(head-i+N)%N;uint8_t v=d==0?255:d==1?190:d==2?120:d==3?60:d==4?20:0;return strip.Color(v,(uint8_t)(v*.25f),(uint8_t)(255-v*.65f));}
  case 11:{uint32_t q=(ms/130+i*37)%29; if(q<3)return strip.Color(random(100,256),random(70,220),random(100,256));return strip.Color(0,0,0);}
  case 12:{v=(uint8_t)constrain((int)(65+random(-24,25)+35*w),25,150);return strip.Color(v,(uint8_t)(v*.42f),(uint8_t)(v*.08f));}
  case 13:{uint32_t q=(ms/120+i*53)%31;return q<3?strip.Color(190,210,255):strip.Color(0,0,0);}
  case 14:{uint32_t q=(ms/90+i*19)%23;v=q<4?(uint8_t)random(90,190):0;return strip.Color(v,(uint8_t)(v*.82f),(uint8_t)(v*.58f));}
  case 15:{float a=(ms/1800.0f+i*.31f);uint8_t r=(uint8_t)(15+25*(1+sinf(a)));uint8_t g=(uint8_t)(35+75*(1+sinf(a+1.4f)));uint8_t b=(uint8_t)(70+85*(1+sinf(a+2.3f)));return strip.Color(r,g,b);}
  case 16:{float h=(float)((ms/35+i*8)%255)/255.0f;uint8_t r,g,b;if(h<.5f){r=(uint8_t)(190-100*h);g=(uint8_t)(35+70*h);b=5;}else{r=(uint8_t)(140-100*(h-.5f));g=(uint8_t)(70-45*(h-.5f));b=5;}return strip.Color(r,g,b);}
  case 17:{int lit=(ms/110)%N;return i<=lit?strip.Color(255,120,25):strip.Color(0,0,0);}
  case 18:{int pos=(ms/110)%3;return ((i+pos)%3)==0?strip.Color(255,180,70):strip.Color(0,0,0);}
  default:return strip.Color(255,180,90);
 }
}
void render(uint32_t ms){ClockInfo c=clockNow();int p=c.valid?phaseOf(c.t):0;uint32_t mask=testMode?testMask:prodMask;int active=manualMode?manualLed:testMode?testActive:prodActive;static float lev[N],tar[N];static uint32_t next[N];static bool init=false;if(!init){for(int i=0;i<N;i++){lev[i]=random(5,30);tar[i]=random(5,60);next[i]=ms+random(300,2000);}init=true;}strip.clear();
 if(rouletteMode){
   uint32_t elapsed=ms-rouletteStart;
   if(elapsed>=rouletteDuration){
     rouletteMode=false;
     rouletteResultMode=true;
     rouletteResultStart=ms;
     manualMode=false;
   } else {
     float progress=(float)elapsed/(float)rouletteDuration;
     // Interval grows quadratically: fast at first, then progressively slower.
     uint32_t interval=45+(uint32_t)(520.0f*progress*progress);
     if((int32_t)(ms-rouletteNext)>=0){
       rouletteLed=random(0,N);
       rouletteNext=ms+interval;
     }
     if(rouletteStyle==1){
       strip.setPixelColor(rouletteLed,strip.Color(255,190,110));
     } else if(rouletteStyle==2){
       // Roulette 2: expanding and contracting golden ring around the selected LED.
       int radius=(elapsed/170)%((N/2)+1);
       for(int i=0;i<N;i++){int d=abs(i-rouletteLed);d=min(d,N-d);if(d==radius)strip.setPixelColor(i,strip.Color(255,150,35));}
       strip.setPixelColor(rouletteLed,strip.Color(255,245,190));
     } else if(rouletteStyle==3){
       // Roulette 3: scanner sweep with a blue/cyan tail.
       int pos=(elapsed/55)%N;
       for(int i=0;i<N;i++){int d=(pos-i+N)%N;uint8_t v=d==0?255:d==1?150:d==2?65:0;if(v)strip.setPixelColor(i,strip.Color(0,(uint8_t)(v*.75f),v));}
       rouletteLed=pos;
     } else {
       // Roulette 4: multicolor sparkle burst around a moving selection.
       for(int i=0;i<N;i++){if(random(0,100)<9)strip.setPixelColor(i,strip.Color(random(70,256),random(40,220),random(80,256)));}
       strip.setPixelColor(rouletteLed,strip.Color(255,255,255));
     }
   }
 }
 else if(rouletteResultMode){
   uint32_t elapsed=ms-rouletteResultStart;
   const uint32_t blinkDuration=1500;
   if(elapsed<blinkDuration){
     bool on=((elapsed/180)%2)==0;
     if(on) strip.setPixelColor(rouletteLed,strip.Color(255,190,110));
   } else {
     uint32_t breatheMs=elapsed-blinkDuration;
     float wave=(1.0f-cosf((breatheMs%2600)/2600.0f*2.0f*PI))*0.5f;
     uint8_t level=(uint8_t)(25.0f+210.0f*wave);
     strip.setPixelColor(rouletteLed,strip.Color(level,(uint8_t)(level*0.75f),(uint8_t)(level*0.42f)));
   }
 }
 else if(manualMode){if(manualLed>=0){uint32_t col=strip.Color(255,190,110);if(manualEffect==1)col=testEffectColor(3,ms,manualLed);else if(manualEffect==2)col=testEffectColor(1,ms,manualLed);else if(manualEffect==3)col=strip.Color(20,100,255);else if(manualEffect==4)col=strip.Color(255,0,0);else if(manualEffect==5)col=strip.Color(0,255,0);else if(manualEffect==6)col=strip.Color(0,0,255);strip.setPixelColor(manualLed,manualEffect==2?testEffectColor(1,ms,manualLed):col);}}
 else if(effectMode){for(int i=0;i<N;i++)strip.setPixelColor(i,testEffectColor(effectType,ms,i));}
 else if(c.valid&&p==2){for(int i=0;i<N;i++){if((int32_t)(ms-next[i])>=0){tar[i]=random(4,70);next[i]=ms+random(400,2300);}lev[i]+=(tar[i]-lev[i])*.035f;uint8_t v=constrain((int)lev[i],0,80);strip.setPixelColor(i,strip.Color(v,v,v*.72));}}
 else if(c.valid&&p==1){int fx=effectForDate(c.t);for(int i=0;i<N;i++){if(!(mask&(1UL<<i)))strip.setPixelColor(i,0);else if(i==active)strip.setPixelColor(i,dailyColor(fx,ms,i));else strip.setPixelColor(i,strip.Color(OPENED_BRIGHTNESS,OPENED_BRIGHTNESS,OPENED_BRIGHTNESS));}}
 // Adventny program pouziva iba LED 0..23. Vsetky dalsie LED
 // sa pri kazdom vykresleni explicitne vynuluju.
 for(int i=N;i<PHYSICAL_LED_COUNT;i++) strip.setPixelColor(i,0,0,0);
 strip.show();}

void loadWiFiNetworks(){
 savedWiFiCount=0;
 uint8_t count=prefs.getUChar("wifiCount",0);
 if(count>MAX_WIFI_NETWORKS)count=MAX_WIFI_NETWORKS;
 for(uint8_t i=0;i<count;i++){
  String sk="w"+String(i)+"s", pk="w"+String(i)+"p";
  String ssid=prefs.getString(sk.c_str(),"");
  String pass=prefs.getString(pk.c_str(),"");
  if(ssid.length()&&savedWiFiCount<MAX_WIFI_NETWORKS){
   savedWiFi[savedWiFiCount].ssid=ssid;
   savedWiFi[savedWiFiCount].password=pass;
   savedWiFiCount++;
  }
 }
}
void saveWiFiNetworks(){
 prefs.putUChar("wifiCount",savedWiFiCount);
 for(uint8_t i=0;i<MAX_WIFI_NETWORKS;i++){
  String sk="w"+String(i)+"s", pk="w"+String(i)+"p";
  if(i<savedWiFiCount){prefs.putString(sk.c_str(),savedWiFi[i].ssid);prefs.putString(pk.c_str(),savedWiFi[i].password);}
  else {prefs.remove(sk.c_str());prefs.remove(pk.c_str());}
 }
}
int findSavedWiFi(const String& ssid){for(uint8_t i=0;i<savedWiFiCount;i++)if(savedWiFi[i].ssid==ssid)return i;return -1;}
void configureTimeSync(){
 if(!timeSyncConfigured){configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3","pool.ntp.org","time.google.com","time.cloudflare.com");timeSyncConfigured=true;}
}
void connectBestSavedWiFi(){
 if(!savedWiFiCount){Serial.println("No saved Wi-Fi networks; configuration AP remains active.");return;}
 int n=WiFi.scanNetworks(false,true);
 int best=-1,bestRssi=-1000;
 for(int i=0;i<n;i++){
  String found=WiFi.SSID(i);
  int ix=findSavedWiFi(found);
  if(ix>=0&&WiFi.RSSI(i)>bestRssi){best=ix;bestRssi=WiFi.RSSI(i);}
 }
 WiFi.scanDelete();
 if(best<0){
  Serial.println("No saved Wi-Fi network found; configuration AP remains active.");
  return;
 }
 Serial.printf("Connecting to saved Wi-Fi: %s (RSSI %d dBm)\\n",savedWiFi[best].ssid.c_str(),bestRssi);
 WiFi.begin(savedWiFi[best].ssid.c_str(),savedWiFi[best].password.c_str());
 uint32_t started=millis();
 while(WiFi.status()!=WL_CONNECTED&&millis()-started<12000)delay(200);
 if(WiFi.status()==WL_CONNECTED){
  connectedSsid=WiFi.SSID();
  Serial.printf("Wi-Fi connected: %s, IP: %s\\n",connectedSsid.c_str(),WiFi.localIP().toString().c_str());
  startMdnsIfNeeded();
  configureTimeSync();
 }else{
  Serial.println("Wi-Fi connection failed; configuration AP remains active.");
  WiFi.disconnect(false,false);
 }
}
String wifiNetworksJson(){
 String out="{\"count\":"+String(savedWiFiCount)+",\"networks\":[";
 for(uint8_t i=0;i<savedWiFiCount;i++){
  if(i)out+=",";
  out+="{\"ssid\":\"";
  String escaped=savedWiFi[i].ssid;
  escaped.replace("\\","\\\\");
  escaped.replace("\"","\\\"");
  out+=escaped;
  out+="\",\"connected\":";
  out+=(WiFi.status()==WL_CONNECTED && WiFi.SSID()==savedWiFi[i].ssid) ? "true" : "false";
  out+="}";
 }
 out+="]}";
 return out;
}

String statusJson(){
 ClockInfo c=clockNow();
 int p=c.valid?phaseOf(c.t):0;
 char dt[24]="No valid time";
 if(c.valid)snprintf(dt,sizeof(dt),"%04d-%02d-%02d %02d:%02d:%02d",
   c.t.tm_year+1900,c.t.tm_mon+1,c.t.tm_mday,c.t.tm_hour,c.t.tm_min,c.t.tm_sec);
 int active=manualMode?manualLed:testMode?testActive:prodActive;
 char stripText[80];
 size_t pos=0;
 pos+=snprintf(stripText+pos,sizeof(stripText)-pos,"ESP");
 uint32_t ms=millis();
 for(int i=0;i<N&&pos+2<sizeof(stripText);i++){
  char ch='.';
  uint32_t mask=testMode?testMask:prodMask;
  if(manualMode)ch=(i==manualLed)?'*':'.';
  else if(effectMode){uint32_t q=(ms/350+i*7)%17;ch=(effectType==1?(i==(ms/800)%N?'*':'.'):(q<3?'.':q<8?':':q<13?'+':'*'));}
  else if(p==1&&(mask&(1UL<<i)))ch=(i==active)?((ms%1400)<700?'+':'*'):'o';
  else if(p==2)ch=(random(0,100)<12)?'*':':';
  stripText[pos++]=ch;
 }
 stripText[pos]='\0';
 char jsonBuf[512];
 snprintf(jsonBuf,sizeof(jsonBuf),
  "{\"timeValid\":%s,\"testMode\":%s,\"manualLedMode\":%s,\"effectTestMode\":%s,\"rouletteMode\":%s,\"rouletteResultMode\":%s,\"rouletteLed\":%d,\"wifi\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"apIp\":\"%s\",\"phase\":\"%s\",\"opened\":%d,\"productionOpened\":%d,\"activeLed\":%d,\"brightness\":%u,\"dailyEffectMode\":%u,\"dailyEffect\":%u,\"dailyEffectSpeed\":%u,\"strip\":\"%s\",\"dateTime\":\"%s\"}",
  c.valid?"true":"false",testMode?"true":"false",manualMode?"true":"false",effectMode?"true":"false",rouletteMode?"true":"false",rouletteResultMode?"true":"false",rouletteLed+1,
  WiFi.status()==WL_CONNECTED?"true":"false",WiFi.status()==WL_CONNECTED?WiFi.SSID().c_str():"",WiFi.status()==WL_CONNECTED?WiFi.localIP().toString().c_str():WiFi.softAPIP().toString().c_str(),WiFi.softAPIP().toString().c_str(),phaseName(p),
  testMode?bits(testMask):bits(prodMask),bits(prodMask),active+1,brightness,dailyEffectMode,dailyEffect,dailyEffectSpeed,stripText,dt);
 return String(jsonBuf);
}
void json(int code,String s){server.sendHeader("Cache-Control","no-store");server.send(code,"application/json; charset=utf-8",s);}
#include "web_page.h"
void sendJson(int c,String s){server.sendHeader("Cache-Control","no-store");server.send(c,"application/json; charset=utf-8",s);}
void setupWeb(){
 server.on("/api/wifi/list",HTTP_GET,[]{sendJson(200,wifiNetworksJson());});
 server.on("/api/wifi/scan",HTTP_GET,[]{
  int n=WiFi.scanNetworks(false,true);
  String out="{\"networks\":[";
  bool first=true;
  for(int i=0;i<n;i++){
   String ssid=WiFi.SSID(i); if(!ssid.length())continue;
   if(!first)out+=",";
   first=false;
   String esc=ssid;esc.replace("\\","\\\\");esc.replace("\"","\\\"");
   out+="{\"ssid\":\""+esc+"\",\"rssi\":"+String(WiFi.RSSI(i))+",\"saved\":"+(findSavedWiFi(ssid)>=0?"true":"false")+"}";
  }
  out+="]}";WiFi.scanDelete();sendJson(200,out);
 });
 server.on("/api/wifi/add",HTTP_POST,[]{
  String ssid=server.arg("ssid"),pass=server.arg("password");
  ssid.trim();
  if(!ssid.length()||ssid.length()>32||pass.length()>63){sendJson(400,"{\"error\":\"SSID alebo heslo ma neplatnu dlzku.\"}");return;}
  int ix=findSavedWiFi(ssid);
  if(ix<0&&savedWiFiCount>=MAX_WIFI_NETWORKS){sendJson(400,"{\"error\":\"Zoznam je plny (max. 5 sieti). Najprv niektoru vymazte.\"}");return;}
  if(ix<0)ix=savedWiFiCount++;
  savedWiFi[ix].ssid=ssid;savedWiFi[ix].password=pass;saveWiFiNetworks();
  sendJson(200,"{\"ok\":true,\"message\":\"Wi-Fi siet ulozena. Pokus o pripojenie sa spusti automaticky.\"}");
  wifiScanRetry=0;
  wifiConfigChanged=true;
 });
 server.on("/api/wifi/delete",HTTP_POST,[]{
  String ssid=server.arg("ssid");int ix=findSavedWiFi(ssid);
  if(ix<0){sendJson(404,"{\"error\":\"Siet sa nenasla v zozname.\"}");return;}
  for(int i=ix;i<savedWiFiCount-1;i++)savedWiFi[i]=savedWiFi[i+1];
  if(savedWiFiCount)savedWiFiCount--;saveWiFiNetworks();
  sendJson(200,"{\"ok\":true,\"message\":\"Wi-Fi siet vymazana.\"}");
 });
 server.on("/",HTTP_GET,[]{server.sendHeader("Cache-Control","no-store");server.send_P(200,"text/html; charset=utf-8",PAGE);});server.on("/api/status",HTTP_GET,[]{sendJson(200,statusJson());});
 server.on("/api/set",HTTP_GET,[]{if(!server.hasArg("dt"))return sendJson(400,"{\"error\":\"Chýba dátum\"}");String s=server.arg("dt");s.replace('T',' ');if(s.length()==16)s+=":00";tm t{};if(!parseDT(s,t))return sendJson(400,"{\"error\":\"Neplatný dátum\"}");testEpoch=mktime(&t);testMode=true;manualMode=effectMode=false;resetTest();pendingTestUpdate=true;sendJson(200,"{\"ok\":true,\"message\":\"Testovací čas prijatý; testovací stav sa aktualizuje.\"}");});
 server.on("/api/tick",HTTP_GET,[]{if(!testMode)return sendJson(400,"{\"error\":\"Najprv nastav testovací dátum.\"}");int d=server.hasArg("days")?server.arg("days").toInt():1;if(d<-366||d>366)return sendJson(400,"{\"error\":\"Posun mimo rozsahu.\"}");testEpoch+=(time_t)d*86400;tm t{};localtime_r(&testEpoch,&t);lastTestDate=-1;processTest(t);manualMode=effectMode=false;sendJson(200,"{\"ok\":true,\"message\":\"Testovací dátum posunutý; produkcia nezmenená.\"}");});
 server.on("/api/auto",HTTP_GET,[]{testMode=manualMode=effectMode=false;lastProdDate=-1;sendJson(200,"{\"ok\":true,\"message\":\"Automatický režim obnovený.\"}");});
 server.on("/api/testreset",HTTP_GET,[]{resetTest();manualMode=effectMode=false;if(testMode){tm t{};localtime_r(&testEpoch,&t);processTest(t);}sendJson(200,"{\"ok\":true,\"message\":\"Test resetovaný; produkcia nezmenená.\"}");});
 server.on("/api/roulette",HTTP_GET,[]{
   rouletteStyle=server.hasArg("style")?(uint8_t)constrain(server.arg("style").toInt(),1,4):1;
   rouletteResultMode=false;
   rouletteMode=true;
   rouletteStart=millis();
   rouletteDuration=(uint32_t)random(5000,8001);
   rouletteNext=rouletteStart;
   rouletteLed=random(0,N);
   manualMode=false;
   effectMode=false;
   testMode=false;
   sendJson(200,"{\"ok\":true,\"message\":\"Roulette started\"}");
 });
 server.on("/api/manual",HTTP_GET,[]{rouletteMode=false;rouletteResultMode=false;int l=server.hasArg("led")?server.arg("led").toInt():0;if(l<0||l>N){sendJson(400,"{\"error\":\"LED must be 0..24\"}");return;}String f=server.hasArg("fx")?server.arg("fx"):"warm";manualEffect=f=="rainbow"?1:f=="pulse"?2:f=="ice"?3:f=="red"?4:f=="green"?5:f=="blue"?6:0;effectMode=false;if(!l){manualMode=false;manualLed=-1;sendJson(200,"{\"ok\":true,\"message\":\"Manual LED test stopped\"}");}else{manualMode=true;manualLed=l-1;sendJson(200,"{\"ok\":true,\"message\":\"LED test started\"}");}});
 server.on("/api/effect",HTTP_GET,[]{String t=server.arg("type");manualMode=false;if(t=="pulse")effectType=1;else if(t=="twinkle")effectType=2;else if(t=="rainbow")effectType=3;else if(t=="aurora")effectType=4;else if(t=="fire")effectType=5;else if(t=="christmas")effectType=6;else if(t=="ice")effectType=7;else if(t=="colorcycle")effectType=8;else if(t=="meteor")effectType=9;else if(t=="comet")effectType=10;else if(t=="fireworks")effectType=11;else if(t=="candle")effectType=12;else if(t=="snowfall")effectType=13;else if(t=="twinklewhite")effectType=14;else if(t=="northernlights")effectType=15;else if(t=="warmsunset")effectType=16;else if(t=="colorwipe")effectType=17;else if(t=="theaterchase")effectType=18;else if(t=="off"){effectMode=false;sendJson(200,"{\"ok\":true,\"message\":\"Effect test stopped\"}");return;}else{sendJson(400,"{\"error\":\"Unknown effect\"}");return;}effectMode=true;testMode=false;sendJson(200,"{\"ok\":true,\"message\":\"Effect test started\"}");});
 server.on("/api/dailyfx",HTTP_GET,[]{
   int mode=dailyEffectMode;
   int effect=dailyEffect;
   int speed=dailyEffectSpeed;
   if(server.hasArg("mode"))mode=server.arg("mode").toInt();
   if(server.hasArg("effect"))effect=server.arg("effect").toInt();
   if(server.hasArg("speed"))speed=server.arg("speed").toInt();
   if(mode<0||mode>2){sendJson(400,"{\"error\":\"Neplatny rezim efektu\"}");return;}
   if(effect<0||effect>8){sendJson(400,"{\"error\":\"Neplatny efekt\"}");return;}
   if(speed<10||speed>100){sendJson(400,"{\"error\":\"Rychlost musi byt 10 az 100\"}");return;}
   dailyEffectMode=(uint8_t)mode;
   dailyEffect=(uint8_t)effect;
   dailyEffectSpeed=(uint8_t)speed;
   saveDailyEffect();
   sendJson(200,"{\"ok\":true,\"message\":\"Nastavenie efektu ulozene.\"}");
  });
 server.on("/api/testfxsettings",HTTP_GET,[]{int sp=server.hasArg("speed")?server.arg("speed").toInt():testEffectSpeed;int br=server.hasArg("brightness")?server.arg("brightness").toInt():brightness;if(sp<10||sp>100||br<1||br>255){sendJson(400,"{\"error\":\"Rychlost 10-100, jas 1-255.\"}");return;}testEffectSpeed=(uint8_t)sp;brightness=(uint8_t)br;strip.setBrightness(brightness);sendJson(200,"{\"ok\":true,\"message\":\"Nastavenia testu pouzite.\"}");});
 server.on("/api/brightness",HTTP_GET,[]{int v=server.hasArg("v")?server.arg("v").toInt():DEFAULT_BRIGHTNESS;if(v<1||v>255)return sendJson(400,"{\"error\":\"Jas musí byť 1 až 255.\"}");brightness=v;strip.setBrightness(brightness);sendJson(200,"{\"ok\":true,\"message\":\"Jas nastavený.\"}");});
 server.onNotFound([]{sendJson(404,"{\"error\":\"Neznáma cesta\"}");});server.begin();}
void startMdnsIfNeeded(){
 if(WiFi.status()==WL_CONNECTED&&!mdnsStarted){
  mdnsStarted=MDNS.begin("advent");
  if(mdnsStarted){MDNS.addService("http","tcp",80);}
  else Serial.println("mDNS failed; use the IP address shown by the router.");
 }
}
void connectWiFi(){
 WiFi.mode(WIFI_AP_STA);
 WiFi.setAutoReconnect(true);
 bool apOk=WiFi.softAP(CONFIG_AP_SSID,CONFIG_AP_PASSWORD);
 Serial.printf("Configuration AP %s: %s, IP: %s\\n",CONFIG_AP_SSID,apOk?"started":"failed",WiFi.softAPIP().toString().c_str());
 connectBestSavedWiFi();
}
void serialCmd(){}
void setup(){delay(50);pinMode(ONBOARD_LED_PIN,OUTPUT);digitalWrite(ONBOARD_LED_PIN,ONBOARD_LED_ACTIVE_LOW?HIGH:LOW);strip.begin();strip.setBrightness(brightness);strip.clear();strip.show();prefs.begin("advent",false);loadWiFiNetworks();clearPlan(prodPlan);clearPlan(testPlan);loadProd();loadDailyEffect();randomSeed(esp_random());connectWiFi();setupWeb();}
void loop(){server.handleClient();if(pendingTestUpdate){pendingTestUpdate=false;tm pendingTm{};localtime_r(&testEpoch,&pendingTm);lastTestDate=-1;processTest(pendingTm);}serialCmd();if(wifiConfigChanged){wifiConfigChanged=false;mdnsStarted=false;connectBestSavedWiFi();}if(WiFi.status()!=WL_CONNECTED&&millis()-lastWifiRetry>30000){lastWifiRetry=millis();mdnsStarted=false;connectBestSavedWiFi();}if(WiFi.status()==WL_CONNECTED&&!mdnsStarted)startMdnsIfNeeded();ClockInfo c=clockNow();ledStatus(c.valid);if(c.valid){if(testMode)processTest(c.t);else processProd(c.t);}uint32_t ms=millis();if(ms-lastAnim>=35){lastAnim=ms;render(ms);}if(ms-lastPrint>=1000){lastPrint=ms;}if(ms-lastStatus>=10000){lastStatus=ms;}}
