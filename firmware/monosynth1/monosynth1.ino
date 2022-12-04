/**
 * monosynth1.ino - picotouchsynth wubwubwub synth using LowPassFilter
 * based MozziScout "mozziscout_monosynth1"
 *
 * Responds to USB MIDI too
 *
 * @todbot 1 Dec 2022
 *
 **/


// Mozzi's controller update rate, seems to have issues at 1024
// If slower than 512 can't get all MIDI from Live
#define CONTROL_RATE 512
// set DEBUG_MIDI 1 to show CCs received in Serial Monitor
#define DEBUG_MIDI 1

#include <MozziGuts.h>
#include <Oscil.h>
#include <tables/triangle_analogue512_int8.h>
#include <tables/square_analogue512_int8.h>
#include <tables/saw_analogue512_int8.h>
#include <tables/cos2048_int8.h> // for filter modulation
#include <LowPassFilter.h>
#include <ADSR.h>
#include <Portamento.h>
#include <mozzi_midi.h> // for mtof()

#include <Adafruit_TinyUSB.h>
#include <MIDI.h>
#include <Adafruit_NeoPixel.h>

#include "TouchyTouch.h"

// SETTINGS
//int portamento_time = 50;  // milliseconds
//int env_release_time = 1000; // milliseconds
byte sound_mode = 0; // patch number / program change
bool retrig_lfo = true;
int midi_base_note = 48;  // for touch keyboard

enum KnownCCs {
  Modulation=0,
  Resonance,
  FilterCutoff,
  PortamentoTime,
  EnvReleaseTime,
  CC_COUNT
};

// mapping of KnownCCs to MIDI CC numbers (this is somewhat standardized)
uint8_t midi_ccs[] = {
  1,   // modulation
  71,  // resonance
  74,  // filter cutoff
  5,   // portamento time
  72,  // env release time
};
uint8_t mod_vals[ CC_COUNT ];

// how picotouchsynth board is set up
const int num_touch = 16;
const int num_leds = 13;
const int touch_pins[] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
const int led_pin      = 16;
const int midi_pin     = 17;
const int disp_sda_pin = 20;
const int disp_scl_pin = 21;
const int audio_pin    = 28; // edit Mozzi/AudioConfigRP2040.h to set this

Adafruit_USBD_MIDI usb_midi;
MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, usb_midi, MIDIusb);

TouchyTouch touches[num_touch];
Adafruit_NeoPixel leds = Adafruit_NeoPixel(num_leds, led_pin, NEO_GRB + NEO_KHZ800);

bool keys_pressed[12]; // one per note

// Oscillators
Oscil<SAW_ANALOGUE512_NUM_CELLS, AUDIO_RATE> aOsc1(SAW_ANALOGUE512_DATA);
Oscil<SAW_ANALOGUE512_NUM_CELLS, AUDIO_RATE> aOsc2(SAW_ANALOGUE512_DATA);
// Filter LFO
Oscil<COS2048_NUM_CELLS, CONTROL_RATE> kFilterMod(COS2048_DATA); // filter mod
// Amplitude envelope
//ADSR <CONTROL_RATE, AUDIO_RATE> envelope;
ADSR <CONTROL_RATE, CONTROL_RATE> envelope;

Portamento <CONTROL_RATE> portamento;
LowPassFilter lpf;


//
// core1 is for UI and MIDI
//

uint8_t fade_amount = 1;

// fade a color by an amount
uint32_t fadeToBlackBy(uint32_t c, uint8_t amount)
{
  uint8_t r = (uint8_t)(c >> 16);
  uint8_t g = (uint8_t)(c >>  8);
  uint8_t b = (uint8_t)c;
  // fade to black by n
  r = (r>amount) ? r - amount : 0;
  g = (g>amount) ? g - amount : 0;
  b = (b>amount) ? b - amount : 0;
  return (r << 16) | (g << 8) | b;
}

/* void setLed(int note, bool on) { */
/*   int n = (note%12); */
/*   keys_pressed[n] = on; */
/*   if( on )  { */
/*     leds.setPixelColor( n+1 , leds.ColorHSV(millis()*10,255,255) ); */
/*   } */
/*   leds.show(); */
/* } */

void setup1() {
  MIDIusb.begin(MIDI_CHANNEL_OMNI);   // Initiate MIDI communications, listen to all channels
  MIDIusb.turnThruOff();    // turn off echo

  // LEDs
  leds.begin();
  leds.setBrightness(51); // 0.2
  leds.show(); // off
  for( int i=0; i < num_leds; i++ ) {
    uint32_t newcolor = leds.ColorHSV(i*255*20);
    leds.setPixelColor(i, newcolor);
  }
  leds.show();

  // Touch buttons
  for(int i=0; i<num_touch; i++) {
    touches[i] = TouchyTouch();
    touches[i].begin( touch_pins[i] );
    touches[i].threshold += 200;
  }
  touches[0].threshold += 1000; // mode button is more sensitive
}

void loop1() {
  handleMIDI();

  // LED fading fade all released note LEDS by same amount
  for ( int i = 0; i < 12; i++) {
    int ledn = i+1;
    uint32_t c = leds.getPixelColor(ledn);
    if( keys_pressed[i] ) {
      c = leds.Color(millis()*20*255, 255,255);
    }
    c = fadeToBlackBy(c, fade_amount);
    leds.setPixelColor(ledn, c);
  }
  leds.show();

  // key handling
  for( int i=0; i<num_touch; i++) {
    touches[i].update();
    //Serial.printf("touch %d %d %d\n", i, touches[i].threshold, touches[i].raw_val_last);

    if( touches[i].rose() ) {
      Serial.printf("pressed %d\n", i);
      if( i==0 ) { // mode button
      }
      else if( i > 0 && i < 13 ) { // notes
        handleNoteOn(0, midi_base_note + i - 1, 100 );
        for(i=0;i<12;i++) { Serial.printf("%d ",keys_pressed[i]); }; Serial.println();
      }
      else if( i== 13 ) { // up
        midi_base_note += 12;
      }
      else if( i== 14 ) { // down
        midi_base_note -= 12;
      }
      else if( i== 15 ) { // select
        sound_mode = (sound_mode + 1) % 3;
        handleProgramChange(sound_mode);
      }
    }

    if( touches[i].fell() ) {
      Serial.printf("released %d\n", i);
      if( i > 0 && i < 13 ) {
        handleNoteOff(0, midi_base_note + i - 1, 100 );
      }
    }
  }
  delay(1); // rate limit, cand probably remove this
}

//
// core0 is for audio synthesis
//

//
void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);

  startMozzi(CONTROL_RATE);

  envelope.setReleaseLevel(0);

  handleProgramChange(0); // set our initial patch
}

//
void loop() {
  audioHook();
}


//
void handleNoteOn(byte channel, byte note, byte velocity) {
  #if DEBUG_MIDI
  Serial.printf("noteOn %d %d\n", note, velocity);
  #endif
  digitalWrite(LED_BUILTIN,HIGH);
  portamento.start(note);
  envelope.noteOn();

  keys_pressed[note%12] = true;
}

//
void handleNoteOff(byte channel, byte note, byte velocity) {
  #if DEBUG_MIDI
  Serial.printf("noteOff %d %d\n", note, velocity);
  #endif
  digitalWrite(LED_BUILTIN,LOW);
  envelope.noteOff();
  keys_pressed[note%12] = false;
}

//
void handleControlChange(byte channel, byte cc_num, byte cc_val) {
  #if DEBUG_MIDI
  Serial.printf("CC %d %d\n", cc_num, cc_val);
  #endif
  for( int i=0; i<CC_COUNT; i++) {
    if( midi_ccs[i] == cc_num ) { // we got one
      mod_vals[i] = cc_val;
      // special cases, not set every updateControl()
      if( i == PortamentoTime ) {
        portamento.setTime( mod_vals[PortamentoTime] * 2);
      }
      else if( i == EnvReleaseTime ) {
        Serial.printf("release time: %d\n", mod_vals[EnvReleaseTime]*10);
        envelope.setReleaseTime( mod_vals[EnvReleaseTime]*10 );
      }
    }
  }
}

//
void handleProgramChange(byte m) {
  Serial.print("program change:"); Serial.println((byte)m);
  sound_mode = m;
  if( sound_mode == 0 ) {
    aOsc1.setTable(SAW_ANALOGUE512_DATA);
    aOsc2.setTable(SAW_ANALOGUE512_DATA);

    mod_vals[Modulation] = 0;   // FIXME: modulation unused currently
    mod_vals[Resonance] = 93;
    mod_vals[FilterCutoff] = 60;
    mod_vals[PortamentoTime] = 50; // actually in milliseconds
    mod_vals[EnvReleaseTime] = 120; // in 10x milliseconds (100 = 1000 msecs)

    lpf.setCutoffFreqAndResonance(mod_vals[FilterCutoff], mod_vals[Resonance]*2);

    kFilterMod.setFreq(4.0f);  // fast
    envelope.setADLevels(255, 255);
    envelope.setTimes(50, 200, 20000, mod_vals[EnvReleaseTime]*10 );
    portamento.setTime( mod_vals[PortamentoTime] );
  }
  else if ( sound_mode == 1 ) {
    aOsc1.setTable(SQUARE_ANALOGUE512_DATA);
    aOsc2.setTable(SQUARE_ANALOGUE512_DATA);

    mod_vals[Resonance] = 50;
    mod_vals[EnvReleaseTime] = 15;

    lpf.setCutoffFreqAndResonance(mod_vals[FilterCutoff], mod_vals[Resonance]*2);

    kFilterMod.setFreq(0.5f);     // slow
    envelope.setADLevels(255, 255);
    envelope.setTimes(50, 100, 20000, (uint16_t)mod_vals[EnvReleaseTime]*10 );
    portamento.setTime( mod_vals[PortamentoTime] );
  }
  else if ( sound_mode == 2 ) {
    aOsc1.setTable(TRIANGLE_ANALOGUE512_DATA);
    aOsc2.setTable(TRIANGLE_ANALOGUE512_DATA);
    mod_vals[FilterCutoff] = 65;
    //kFilterMod.setFreq(0.25f);    // slower
    //retrig_lfo = false;
  }
}

//
void handleMIDI() {
  while( MIDIusb.read() ) {  // use while() to read all pending MIDI, shouldn't hang
    switch(MIDIusb.getType()) {
      case midi::ProgramChange:
        handleProgramChange(MIDIusb.getData1());
        break;
      case midi::ControlChange:
        handleControlChange(0, MIDIusb.getData1(), MIDIusb.getData2());
        break;
      case midi::NoteOn:
        handleNoteOn( 0, MIDIusb.getData1(),MIDIusb.getData2());
        break;
      case midi::NoteOff:
        handleNoteOff( 0, MIDIusb.getData1(),MIDIusb.getData2());
        break;
      default:
        break;
    }
  }
}

byte envgain;  // do envelope in updateControl() instead of in updateAudio()

// mozzi function, called at CONTROL_RATE times per second
void updateControl() {
  //handleMIDI();

  // map the lpf modulation into the filter range (0-255), corresponds with 0-8191Hz, kFilterMod runs -128-127
  //uint8_t cutoff_freq = cutoff + (mod_amount * (kFilterMod.next()/2));
  //  uint16_t fm = ((kFilterMod.next() * mod_vals[Modulation]) / 128) + 127 ;
  //  uint8_t cutoff_freq = constrain(mod_vals[FilterCutoff] + fm, 0,255 );

  //  lpf.setCutoffFreqAndResonance(cutoff_freq, mod_vals[Resonance]*2);

  lpf.setCutoffFreqAndResonance(mod_vals[FilterCutoff], mod_vals[Resonance]*2);  // don't *2 filter since we want 0-4096Hz

  envelope.update();
  envgain = envelope.next(); // this is where it's different to an audio rate envelope

  Q16n16 pf = portamento.next();  // Q16n16 is a fixed-point fraction in 32-bits (16bits . 16bits)
  aOsc1.setFreq_Q16n16(pf);
  aOsc2.setFreq_Q16n16(pf*1.015);

}

// mozzi function, called at AUDIO_RATE times per second
AudioOutput_t updateAudio() {
  long asig = lpf.next( aOsc1.next() + aOsc2.next() );
  return MonoOutput::fromAlmostNBit(18, envgain * asig); // 16 = 8 signal bits + 8 envelope bits
  //  return MonoOutput::fromAlmostNBit(18, envelope.next() * asig); // 16 = 8 signal bits + 8 envelope bits
}
