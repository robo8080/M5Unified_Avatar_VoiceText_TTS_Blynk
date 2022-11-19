#include <M5UnitLCD.h>
#include <M5UnitOLED.h>
#include <M5Unified.h>
#include <Avatar.h>

#include <AudioOutput.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include "AudioFileSourceVoiceTextStream.h"

#include <BlynkSimpleEsp32.h> // Blynk for esp32

using namespace m5avatar;

Avatar avatar;

const char *SSID = "YOUR_WIFI_SSID";
const char *PASSWORD = "YOUR_WIFI_PASSWORD";

// #Blynk
// You should get Auth Token.
// visit https://docs.blynk.cc/
const char* auth = "YOUR_BLYNK_AUTH_TOKEN";

/// set M5Speaker virtual channel (0-7)
static constexpr uint8_t m5spk_virtual_channel = 0;

class AudioOutputM5Speaker : public AudioOutput
{
  public:
    AudioOutputM5Speaker(m5::Speaker_Class* m5sound, uint8_t virtual_sound_channel = 0)
    {
      _m5sound = m5sound;
      _virtual_ch = virtual_sound_channel;
    }
    virtual ~AudioOutputM5Speaker(void) {};
    virtual bool begin(void) override { return true; }
    virtual bool ConsumeSample(int16_t sample[2]) override
    {
      if (_tri_buffer_index < tri_buf_size)
      {
        _tri_buffer[_tri_index][_tri_buffer_index  ] = sample[0];
        _tri_buffer[_tri_index][_tri_buffer_index+1] = sample[0];
        _tri_buffer_index += 2;

        return true;
      }

      flush();
      return false;
    }
    virtual void flush(void) override
    {
      if (_tri_buffer_index)
      {
        _m5sound->playRaw(_tri_buffer[_tri_index], _tri_buffer_index, hertz, true, 1, _virtual_ch);
        _tri_index = _tri_index < 2 ? _tri_index + 1 : 0;
        _tri_buffer_index = 0;
        ++_update_count;
      }
    }
    virtual bool stop(void) override
    {
      flush();
      _m5sound->stop(_virtual_ch);
      for (size_t i = 0; i < 3; ++i)
      {
        memset(_tri_buffer[i], 0, tri_buf_size * sizeof(int16_t));
      }
      ++_update_count;
      return true;
    }

    const int16_t* getBuffer(void) const { return _tri_buffer[(_tri_index + 2) % 3]; }
    const uint32_t getUpdateCount(void) const { return _update_count; }

  protected:
    m5::Speaker_Class* _m5sound;
    uint8_t _virtual_ch;
    static constexpr size_t tri_buf_size = 640;
    int16_t _tri_buffer[3][tri_buf_size];
    size_t _tri_buffer_index = 0;
    size_t _tri_index = 0;
    size_t _update_count = 0;
};


//static constexpr const int preallocateBufferSize = 5 * 1024;
//static constexpr const int preallocateCodecSize = 29192; // MP3 codec max mem needed
AudioGeneratorMP3 *mp3;
AudioFileSourceVoiceTextStream *file;
AudioFileSourceBuffer *buff;
static AudioOutputM5Speaker out(&M5.Speaker, m5spk_virtual_channel);
const int preallocateBufferSize = 40*1024;
uint8_t *preallocateBuffer;

// Called when a metadata event occurs (i.e. an ID3 tag, an ICY block, etc.
void MDCallback(void *cbData, const char *type, bool isUnicode, const char *string)
{
  const char *ptr = reinterpret_cast<const char *>(cbData);
  (void) isUnicode; // Punt this ball for now
  // Note that the type and string may be in PROGMEM, so copy them to RAM for printf
  char s1[32], s2[64];
  strncpy_P(s1, type, sizeof(s1));
  s1[sizeof(s1)-1]=0;
  strncpy_P(s2, string, sizeof(s2));
  s2[sizeof(s2)-1]=0;
  Serial.printf("METADATA(%s) '%s' = '%s'\n", ptr, s1, s2);
  Serial.flush();
}

// Called when there's a warning or error (like a buffer underflow or decode hiccup)
void StatusCallback(void *cbData, int code, const char *string)
{
  const char *ptr = reinterpret_cast<const char *>(cbData);
  // Note that the string may be in PROGMEM, so copy it to RAM for printf
  char s1[64];
  strncpy_P(s1, string, sizeof(s1));
  s1[sizeof(s1)-1]=0;
  Serial.printf("STATUS(%s) '%d' = '%s'\n", ptr, code, s1);
  Serial.flush();
}

void lipSync(void *args)
{
  float gazeX, gazeY;
  int level = 0;
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  for (;;)
  {
    level = abs(*out.getBuffer());
    if(level<100) level = 0;
    if(level > 15000)
    {
      level = 15000;
    }
    float open = (float)level/15000.0;
    avatar->setMouthOpenRatio(open);
    avatar->getGaze(&gazeY, &gazeX);
//    avatar->setRotation(gazeX * (5.0/180.0)*3.14);
    delay(50);
  }
}

void setup()
{
  auto cfg = M5.config();

  cfg.external_spk = true;    /// use external speaker (SPK HAT / ATOMIC SPK)
//cfg.external_spk_detail.omit_atomic_spk = true; // exclude ATOMIC SPK
//cfg.external_spk_detail.omit_spk_hat    = true; // exclude SPK HAT

  M5.begin(cfg);

  preallocateBuffer = (uint8_t *)malloc(preallocateBufferSize);
  if (!preallocateBuffer) {
    M5.Display.printf("FATAL ERROR:  Unable to preallocate %d bytes for app\n", preallocateBufferSize);
    for (;;) { delay(1000); }
  }

  { /// custom setting
    auto spk_cfg = M5.Speaker.config();
    /// Increasing the sample_rate will improve the sound quality instead of increasing the CPU load.
    spk_cfg.sample_rate = 96000; // default:64000 (64kHz)  e.g. 48000 , 50000 , 80000 , 96000 , 100000 , 128000 , 144000 , 192000 , 200000
    spk_cfg.task_pinned_core = APP_CPU_NUM;
    M5.Speaker.config(spk_cfg);
  }
  M5.Speaker.begin();

  Serial.println("Connecting to WiFi");
  M5.Lcd.print("Connecting to WiFi");
  WiFi.disconnect();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  Blynk.begin(auth, SSID, PASSWORD);
  Serial.println("\nConnected");
  M5.Lcd.println("\nConnected");
  
  audioLogger = &Serial;

  mp3 = new AudioGeneratorMP3();
  mp3->RegisterStatusCB(StatusCallback, (void*)"mp3");

  avatar.init();
  avatar.addTask(lipSync, "lipSync");

  M5.Speaker.setChannelVolume(m5spk_virtual_channel, 200);
}

char *text1 = "私の名前はスタックチャンです、よろしくね。";
char *text2 = "こんにちは、世界！";
char *tts_parms1 ="&emotion_level=2&emotion=happiness&format=mp3&speaker=hikari&volume=200&speed=120&pitch=130";
char *tts_parms2 ="&emotion_level=2&emotion=happiness&format=mp3&speaker=takeru&volume=200&speed=100&pitch=130";
char *tts_parms3 ="&emotion_level=4&emotion=anger&format=mp3&speaker=bear&volume=200&speed=120&pitch=100";

void VoiceText_tts(char *text,char *tts_parms) {
    file = new AudioFileSourceVoiceTextStream( text, tts_parms);
    buff = new AudioFileSourceBuffer(file, preallocateBuffer, preallocateBufferSize);
    delay(100);
    mp3->begin(buff, &out);
}

// Blynk 
// use 'ping blynk-cloud.com' to find ip address
// http://188.166.206.43/YOUR_BLYNK_AUTH_TOKEN/update/v1?value=hello%20world
BLYNK_WRITE(V1)
{
  String pinValue = param.asStr();
  char text[pinValue.length() + 1];
  pinValue.toCharArray(text, pinValue.length() + 1);
  
  Serial.print("BLYNK_WRITE(V1): ");
  Serial.println(pinValue);
  
  if(!mp3->isRunning()) {    
    avatar.setExpression(Expression::Happy);
    VoiceText_tts(text, tts_parms1);
    avatar.setExpression(Expression::Neutral);
  }

}

// Blynk 
// use 'ping blynk-cloud.com' to find ip address
// http://188.166.206.43/YOUR_BLYNK_AUTH_TOKEN/update/v2?value=hello%20world
BLYNK_WRITE(V2)
{
  String pinValue = param.asStr();
  char text[pinValue.length() + 1];
  pinValue.toCharArray(text, pinValue.length() + 1);
  
  Serial.print("BLYNK_WRITE(V2): ");
  Serial.println(pinValue);
  
  if(!mp3->isRunning()) {    
    avatar.setExpression(Expression::Happy);
    VoiceText_tts(text, tts_parms2);
    avatar.setExpression(Expression::Neutral);
  }
  
}

// Blynk 
// use 'ping blynk-cloud.com' to find ip address
// http://188.166.206.43/YOUR_BLYNK_AUTH_TOKEN/update/v3?value=hello%20world
BLYNK_WRITE(V3)
{
  String pinValue = param.asStr();
  char text[pinValue.length() + 1];
  pinValue.toCharArray(text, pinValue.length() + 1);
  
  Serial.print("BLYNK_WRITE(V3): ");
  Serial.println(pinValue);
  
  if(!mp3->isRunning()) {    
    avatar.setExpression(Expression::Happy);
    VoiceText_tts(text, tts_parms3);
    avatar.setExpression(Expression::Neutral);
  }
  
}

void loop()
{
  static int lastms = 0;
//  Blynk.run();
  M5.update();
  if (M5.BtnA.wasPressed())
  {
    avatar.setExpression(Expression::Happy);
    VoiceText_tts(text1, tts_parms1);
    avatar.setExpression(Expression::Neutral);
    Serial.println("mp3 begin");
  }
  if (M5.BtnB.wasPressed())
  {
    avatar.setExpression(Expression::Happy);
    VoiceText_tts(text1, tts_parms2);
    avatar.setExpression(Expression::Neutral);
    Serial.println("mp3 begin");
  }
  if (M5.BtnC.wasPressed())
  {
    avatar.setExpression(Expression::Happy);
    VoiceText_tts(text1, tts_parms3);
    avatar.setExpression(Expression::Neutral);
    Serial.println("mp3 begin");
  }
  if (mp3->isRunning()) {
    if (millis()-lastms > 1000) {
      lastms = millis();
      Serial.printf("Running for %d ms...\n", lastms);
      Serial.flush();
     }
    if (!mp3->loop()) {
      mp3->stop();
//      out->setLevel(0);
      delete file;
      delete buff;
      Serial.println("mp3 stop");
    }
  } else {
    Blynk.run();
  }
}
