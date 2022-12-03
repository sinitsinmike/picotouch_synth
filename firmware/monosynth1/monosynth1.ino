/**
 * monosynth1 wubwubwub synth using LowPassFilter
 * based MozziScout "mozziscout_monosynth1"
 *
 * Responds to Serial (DIN) MIDI In
 *
 *  @todbot 3 Jan 2021
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
Adafruit_USBD_MIDI usb_midi;
MIDI_CREATE_INSTANCE(Adafruit_USBD_MIDI, usb_midi, MIDIusb);


// SETTINGS
//int portamento_time = 50;  // milliseconds
//int env_release_time = 1000; // milliseconds
byte sound_mode = 0; // patch number / program change
bool retrig_lfo = true;

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

//struct MySettings : public MIDI_NAMESPACE::DefaultSettings {
//  static const bool Use1ByteParsing = false; // Allow MIDI.read to handle all received data in one go
//  static const long BaudRate = 31250;        // Doesn't build without this...
//};
//MIDI_CREATE_CUSTOM_INSTANCE(HardwareSerial, Serial1, MIDI, MySettings); // for USB-based SAMD
//MIDI_CREATE_DEFAULT_INSTANCE();

//
Oscil<SAW_ANALOGUE512_NUM_CELLS, AUDIO_RATE> aOsc1(SAW_ANALOGUE512_DATA);
Oscil<SAW_ANALOGUE512_NUM_CELLS, AUDIO_RATE> aOsc2(SAW_ANALOGUE512_DATA);

Oscil<COS2048_NUM_CELLS, CONTROL_RATE> kFilterMod(COS2048_DATA); // filter mod

//ADSR <CONTROL_RATE, AUDIO_RATE> envelope;
ADSR <CONTROL_RATE, CONTROL_RATE> envelope;

Portamento <CONTROL_RATE> portamento;
LowPassFilter lpf;

#include "FakeyTouch.h"

const int num_touch = 16;
const int touch_pins[num_touch] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };

FakeyTouch touches[num_touch];

//const int touchpin_F = 0; // GP16
//FakeyTouch touchF = FakeyTouch( touchpin_F );

void setup1() {
  for(int i=0; i<num_touch; i++) {
    touches[i] = FakeyTouch( touch_pins[i] );
    touches[i].begin();
  }
  touches[0].threshold = touches[0].threshold + 1000;
}

void loop1() {
  for( int i=0; i<num_touch; i++) { 
    if( touches[i].isTouched() ) { 
      Serial.printf("touch %d %d %d\n", i, touches[i].threshold, touches[i].raw_val_last);
    }
  }
  delay(10);
  
//  if( touchF.isTouched() ) { 
//    Serial.print("TOUCH!\n");
//  }
//  delay(100);
}

//
void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);

  MIDIusb.begin(MIDI_CHANNEL_OMNI);   // Initiate MIDI communications, listen to all channels
  MIDIusb.turnThruOff();    // turn off echo

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
//  Serial.println("midi_test handleNoteOn!");
  #if DEBUG_MIDI 
  Serial.printf("noteOn %d %d\n", note, velocity);
  #endif
  digitalWrite(LED_BUILTIN,HIGH);
  portamento.start(note);
  envelope.noteOn();
}

//
void handleNoteOff(byte channel, byte note, byte velocity) {
  #if DEBUG_MIDI 
  Serial.printf("noteOff %d %d\n", note, velocity);
  #endif
  digitalWrite(LED_BUILTIN,LOW);
  envelope.noteOff();
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


byte envgain;

// mozzi function, called at CONTROL_RATE times per second
void updateControl() {
  handleMIDI();
  
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
  aOsc2.setFreq_Q16n16(pf*1.02);

}

// mozzi function, called at AUDIO_RATE times per second
AudioOutput_t updateAudio() {
  long asig = lpf.next( aOsc1.next() + aOsc2.next() );
  return MonoOutput::fromAlmostNBit(18, envgain * asig); // 16 = 8 signal bits + 8 envelope bits
//  return MonoOutput::fromAlmostNBit(18, envelope.next() * asig); // 16 = 8 signal bits + 8 envelope bits
}
