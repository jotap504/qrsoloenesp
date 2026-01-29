#ifndef SOUND_MANAGER_H
#define SOUND_MANAGER_H

#include <Arduino.h>
#include "Audio.h" // Requires 'ESP32-audioI2S' library by Schreibfaul1

// Default Pin mapping for JC3248W535 (Guition ESP32-S3 3.5" with Audio)
// Confirmed Pinout: BCLK=42, LRC=2, DOUT=41
#define I2S_BCLK_PIN 42
#define I2S_LRC_PIN  2
#define I2S_DOUT_PIN 41 

class SoundManager {
public:
    SoundManager();
    void begin();
    void playStartupSound();
    void loop(); // Must be called in main loop
    
    void playClick();
    void playQrGenerated();
    void playServiceActivated();
    void playSuccess();
    void playWarning();
    void playError();
    
    Audio* getAudio() { return &audio; }

private:
    Audio audio;
};

#endif
