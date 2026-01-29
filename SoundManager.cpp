#include "SoundManager.h"

SoundManager::SoundManager() {}

void SoundManager::begin() {
    // Setup I2S Pins
    audio.setPinout(I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DOUT_PIN);
    
    // Set Volume (0-21)
    audio.setVolume(21); // Max volume
}

void SoundManager::loop() {
    audio.loop();
}

void SoundManager::playStartupSound() {
    audio.connecttohost("https://translate.google.com/translate_tts?ie=UTF-8&q=Sistema%20Iniciado&tl=es&client=tw-ob");
}

void SoundManager::playClick() {
    // Emptied to remove button click lag as requested by user.
}

void SoundManager::playQrGenerated() {
    audio.connecttohost("https://translate.google.com/translate_tts?ie=UTF-8&q=QR%20generado&tl=es&client=tw-ob");
}

void SoundManager::playServiceActivated() {
    audio.connecttohost("https://translate.google.com/translate_tts?ie=UTF-8&q=Servicio%20activado&tl=es&client=tw-ob");
}

void SoundManager::playSuccess() {
    audio.connecttohost("https://translate.google.com/translate_tts?ie=UTF-8&q=Pago%20Aprobado&tl=es&client=tw-ob");
}

void SoundManager::playWarning() {
    audio.connecttohost("https://translate.google.com/translate_tts?ie=UTF-8&q=Un%20minuto%20restante&tl=es&client=tw-ob");
}

void SoundManager::playError() {
    audio.connecttohost("https://translate.google.com/translate_tts?ie=UTF-8&q=Error&tl=es&client=tw-ob");
}
