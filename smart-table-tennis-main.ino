/* Smart Table Tennis Bat - Sliding Window with 250ms hop
Arduino Nano 33 BLE Sense Rev2
Edge Impulse model + enhanced UI/UX + acceleration threshold
Implements overlapping inference windows with half-hop (250ms)
*/

#include <Wire.h>
#include <Arduino_BMI270_BMM150.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <table_tennis_2_inferencing.h>

// Display setup
#define SCREEN_WIDTH       128
#define SCREEN_HEIGHT      32
#define OLED_RESET         -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Timing & thresholds
#define SAMPLE_INTERVAL_MS    20    // 50Hz sampling
#define WINDOW_SIZE           EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE
#define AXIS_COUNT            EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME
#define WINDOW_SAMPLES        (WINDOW_SIZE / AXIS_COUNT)  // e.g. 25
#define HOP_SAMPLES           ((WINDOW_SAMPLES + 1) / 2)   // ~half window (~12 or 13)
#define CONF_THRESH           0.50f
#define ACCEL_THRESH_G        2.0f
#define FLASH_DURATION_MS     3000
#define IDLE_TIMEOUT_MS       10000
#define ANIM_INTERVAL_MS      200
#define CONVERT_G_TO_MS2      9.80665f

const uint8_t IDLE_LABEL_INDEX = EI_CLASSIFIER_LABEL_COUNT - 1;

// Animation
#define BALL_COUNT 5
const uint8_t ballX[BALL_COUNT] = {16, 40, 64, 88, 112};
const uint8_t ballYBase = 22, ballYUp = 18;

enum State { STATE_OPENING, STATE_MAIN, STATE_FLASH, STATE_IDLE };
State currentState = STATE_OPENING;

// Buffers & counters
typedef float BufferRow[AXIS_COUNT];
static BufferRow imuBuffer[WINDOW_SAMPLES];
static uint16_t bufIndex = 0;
static uint16_t sampleCount = 0;    // counts until window full
static uint16_t sampleCounter = 0;  // counts since last inference

static uint16_t countBHdrive=0, countBHsmash=0;
static uint16_t countFHdrive=0, countFHloop=0, countFHsmash=0;
static uint16_t totalStrokes = 0;

// Timing
uint32_t lastIMUread=0, lastDisplay=0, lastIdleAnim=0;
uint32_t lastStrokeTime=0, flashStart=0;

// Encouragement messages
const char* encMsgs[5][2] = {
{“Solid backhand!”,“Clean drive!”},
{“Backhand power!”,“What a backhand!”},
{“Nice drive!”,“Perfect contact!”},
{“Heavy topspin!”,“Great loop!”},
{“Pure power!”,“Smashed it!”}
};
uint8_t encCount[5] = {2,2,2,2,2}, encIndex[5] = {0};

// Inference result
typedef struct { uint8_t index; float confidence; } InferenceResult;

// Helpers
bool accelAboveThreshold() {
float thr = ACCEL_THRESH_G * CONVERT_G_TO_MS2;
float thr2 = thrthr;
for(uint16_t i=0;i<WINDOW_SAMPLES;i++){
float ax=imuBuffer[i][0], ay=imuBuffer[i][1], az=imuBuffer[i][2];
if((axax+ayay+azaz)>thr2) return true;
}
return false;
}

void readIMU(){
float ax=0,ay=0,az=0;
if(IMU.accelerationAvailable()) IMU.readAcceleration(ax,ay,az);
float gx=0,gy=0,gz=0;
if(IMU.gyroscopeAvailable())    IMU.readGyroscope(gx,gy,gz);
BufferRow &r = imuBuffer[bufIndex];
r[0]=axCONVERT_G_TO_MS2; r[1]=ayCONVERT_G_TO_MS2; r[2]=az*CONVERT_G_TO_MS2;
r[3]=gx; r[4]=gy; r[5]=gz;
bufIndex = (bufIndex+1)%WINDOW_SAMPLES;
if(sampleCount<WINDOW_SAMPLES) sampleCount++;
sampleCounter++;
}

InferenceResult runInference(){
static float sig[WINDOW_SIZE];
for(uint16_t i=0;i<WINDOW_SAMPLES;i++){
uint16_t p=(bufIndex+i)%WINDOW_SAMPLES;
for(uint8_t j=0;j<AXIS_COUNT;j++) sig[i*AXIS_COUNT+j]=imuBuffer[p][j];
}
signal_t signal;
if(numpy::signal_from_buffer(sig,WINDOW_SIZE,&signal)!=0) return {0,0.0f};
ei_impulse_result_t res;
if(run_classifier(&signal,&res,false)!=EI_IMPULSE_OK) return {0,0.0f};
size_t best=0;
for(size_t i=1;i<EI_CLASSIFIER_LABEL_COUNT;i++)
if(res.classification[i].value>res.classification[best].value) best=i;
return {(uint8_t)best,res.classification[best].value};
}

void drawMainScreen(){
display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
// BH drive
display.fillRect(0,2,5,2,SSD1306_WHITE); display.setCursor(8,0); display.print(“drive “); display.print(countBHdrive);
// FH drive
display.fillRect(64,2,5,2,SSD1306_WHITE); display.setCursor(71,0); display.print(“drive “); display.print(countFHdrive);
// BH smash
display.drawTriangle(0,5,2,0,5,5,SSD1306_WHITE); display.setCursor(8,8); display.print(“smash “); display.print(countBHsmash);
// FH smash
display.drawTriangle(64,13,66,8,69,13,SSD1306_WHITE); display.setCursor(71,8); display.print(“smash “); display.print(countFHsmash);
// FH loop
display.drawCircle(66,18,2,SSD1306_WHITE); display.setCursor(71,16); display.print(“loop  “); display.print(countFHloop);
// Total
display.setCursor(0,24); display.print(“Total: “); display.print(totalStrokes);
display.display();
}

void drawFlash(uint8_t idx){
display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
uint8_t m = encIndex[idx]++ % encCount[idx]; const char* msg = encMsgs[idx][m];
int16_t x=(SCREEN_WIDTH-strlen(msg)*6)/2; display.setCursor(x,12); display.print(msg); display.display();
}

void drawIdle(){
display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
const char* msg=“Ready? Swing!”; int16_t x=(SCREEN_WIDTH-strlen(msg)*6)/2;
display.setCursor(x,8); display.print(msg);
bool up=((millis()-lastIdleAnim)/ANIM_INTERVAL_MS)&1;
int y= up?ballYUp:ballYBase;
for(uint8_t i=0;i<BALL_COUNT;i++) display.fillCircle(ballX[i],y,2,SSD1306_WHITE);
display.display();
}

void playAnim(){ for(uint8_t f=0;f<4;f++){ display.clearDisplay(); int y=(f&1)?ballYUp:ballYBase; for(uint8_t i=0;i<BALL_COUNT;i++) display.fillCircle(ballX[i],y,2,SSD1306_WHITE); display.display(); delay(ANIM_INTERVAL_MS);} }

void setup(){
Serial.begin(115200);
if(!IMU.begin()) while(1);
if(!display.begin(SSD1306_SWITCHCAPVCC,0x3C)) while(1);
display.clearDisplay(); playAnim();
lastIMUread=millis(); lastDisplay=millis(); lastIdleAnim=millis(); lastStrokeTime=millis();
currentState=STATE_MAIN;
}

void loop(){ uint32_t now=millis();
if(now-lastIMUread>=SAMPLE_INTERVAL_MS){ lastIMUread=now; readIMU(); }
switch(currentState){
case STATE_MAIN:
if(sampleCount>=WINDOW_SAMPLES && sampleCounter>=HOP_SAMPLES){
sampleCounter-=HOP_SAMPLES;
InferenceResult inf=runInference();
if(inf.confidence>CONF_THRESH && inf.index<IDLE_LABEL_INDEX && accelAboveThreshold()){
switch(inf.index){case 0:countBHdrive++;break;case1:countBHsmash++;break;case2:countFHdrive++;break;case3:countFHloop++;break;case4:countFHsmash++;break;}
totalStrokes++; flashStart=now; drawFlash(inf.index); currentState=STATE_FLASH;
}
}
if(now-lastStrokeTime>=IDLE_TIMEOUT_MS){ currentState=STATE_IDLE; lastIdleAnim=now; }
if(now-lastDisplay>=ANIM_INTERVAL_MS){ lastDisplay=now; drawMainScreen(); }
break;

case STATE_FLASH:
  if(sampleCount>=WINDOW_SAMPLES && sampleCounter>=HOP_SAMPLES){
    sampleCounter-=HOP_SAMPLES;
    InferenceResult inf=runInference();
    if(inf.confidence>CONF_THRESH && inf.index<IDLE_LABEL_INDEX && accelAboveThreshold()){
      switch(inf.index){case0:countBHdrive++;break;case1:countBHsmash++;break;case2:countFHdrive++;break;case3:countFHloop++;break;case4:countFHsmash++;break;}
      totalStrokes++; flashStart=now; drawFlash(inf.index);
    }
  }
  if(now-flashStart>=FLASH_DURATION_MS){ lastDisplay=now; lastStrokeTime=now; drawMainScreen(); currentState=STATE_MAIN; }
  break;

case STATE_IDLE:
  if(now-lastIdleAnim>=ANIM_INTERVAL_MS){ lastIdleAnim=now; drawIdle(); }
  if(sampleCount>=WINDOW_SAMPLES && sampleCounter>=HOP_SAMPLES){
    sampleCounter-=HOP_SAMPLES;
    InferenceResult inf=runInference();
    if(inf.confidence>CONF_THRESH && inf.index<IDLE_LABEL_INDEX && accelAboveThreshold()){
      switch(inf.index){case0:countBHdrive++;break;case1:countBHsmash++;break;case2:countFHdrive++;break;case3:countFHloop++;break;case4:countFHsmash++;break;}
      totalStrokes++; flashStart=now; drawFlash(inf.index); currentState=STATE_FLASH;
    }
  }
  break;

default:
  currentState=STATE_MAIN;

}
}