#include "Audio.h"
#include "synth.h"
#include "hostVmShared.h"

#include <string>
#include <sstream>
#include <algorithm> // std::max
#include <cmath>
#include <float.h> // std::max

//playback implemenation based on zetpo 8's
//https://github.com/samhocevar/zepto8/blob/master/src/pico8/sfx.cpp

Audio::Audio(PicoRam* memory){
    _memory = memory;

    for(int i = 0; i < 4; i++) {
        _memory->_sfxChannels[i].sfxId = -1;
    }
}

void Audio::api_sfx(uint8_t sfx, int channel, int offset){

    if (sfx < -2 || sfx > 63 || channel < -1 || channel > 3 || offset > 31) {
        return;
    }

    if (sfx == -1)
    {
        // Stop playing the current channel
        if (channel != -1) {
            _memory->_sfxChannels[channel].sfxId = -1;
        }
    }
    else if (sfx == -2)
    {
        // Stop looping the current channel
        if (channel != -1) {
            _memory->_sfxChannels[channel].can_loop = false;
        }
    }
    else
    {
        // Find the first available channel: either a channel that plays
        // nothing, or a channel that is already playing this sample (in
        // this case PICO-8 decides to forcibly reuse that channel, which
        // is reasonable)
        if (channel == -1)
        {
            for (int i = 0; i < 4; ++i)
                if (_memory->_sfxChannels[i].sfxId == -1 ||
                    _memory->_sfxChannels[i].sfxId == sfx)
                {
                    channel = i;
                    break;
                }
        }

        // If still no channel found, the PICO-8 strategy seems to be to
        // stop the sample with the lowest ID currently playing
        if (channel == -1)
        {
            for (int i = 0; i < 4; ++i) {
               if (channel == -1 || _memory->_sfxChannels[i].sfxId < _memory->_sfxChannels[channel].sfxId) {
                   channel = i;
               }
            }
        }

        // Stop any channel playing the same sfx
        for (int i = 0; i < 4; ++i) {
            if (_memory->_sfxChannels[i].sfxId == sfx) {
                _memory->_sfxChannels[i].sfxId = -1;
            }
        }

        // Play this sound!
        _memory->_sfxChannels[channel].sfxId = sfx;
        _memory->_sfxChannels[channel].offset = std::max(0.f, (float)offset);
        _memory->_sfxChannels[channel].phi = 0.f;
        _memory->_sfxChannels[channel].can_loop = true;
        _memory->_sfxChannels[channel].is_music = false;
        // Playing an instrument starting with the note C-2 and the
        // slide effect causes no noticeable pitch variation in PICO-8,
        // so I assume this is the default value for “previous key”.
        _memory->_sfxChannels[channel].prev_key = 24;
        // There is no default value for “previous volume”.
        _memory->_sfxChannels[channel].prev_vol = 0.f;
    }      
}

void Audio::api_music(uint8_t pattern, int16_t fade_len, int16_t mask){
    if (pattern < -1 || pattern > 63) {
        return;
    }

    if (pattern == -1)
    {
        // Music will stop when fade out is finished
        _memory->_musicChannel.volume_step = fade_len <= 0 ? -FLT_MAX
                                  : -_memory->_musicChannel.volume * (1000.f / fade_len);
        return;
    }

    _memory->_musicChannel.count = 0;
    _memory->_musicChannel.mask = mask ? mask & 0xf : 0xf;

    _memory->_musicChannel.volume = 1.f;
    _memory->_musicChannel.volume_step = 0.f;
    if (fade_len > 0)
    {
        _memory->_musicChannel.volume = 0.f;
        _memory->_musicChannel.volume_step = 1000.f / fade_len;
    }

    set_music_pattern(pattern);
}

void Audio::set_music_pattern(int pattern) {
    _memory->_musicChannel.pattern = pattern;
    _memory->_musicChannel.offset = 0;

    //array to access song's channels. may be better to have this part of the struct?
    uint8_t channels[] = {
        _memory->songs[pattern].sfx0,
        _memory->songs[pattern].sfx1,
        _memory->songs[pattern].sfx2,
        _memory->songs[pattern].sfx3,
    };

    // Find music speed; it’s the speed of the fastest sfx
    _memory->_musicChannel.master = _memory->_musicChannel.speed = -1;
    for (int i = 0; i < 4; ++i)
    {
        uint8_t n = channels[i];

        if (n & 0x40)
            continue;

        auto &sfx = _memory->sfx[n & 0x3f];
        if (_memory->_musicChannel.master == -1 || _memory->_musicChannel.speed > sfx.speed)
        {
            _memory->_musicChannel.master = i;
            _memory->_musicChannel.speed = std::max(1, (int)sfx.speed);
        }
    }

    // Play music sfx on active channels
    for (int i = 0; i < 4; ++i)
    {
        if (((1 << i) & _memory->_musicChannel.mask) == 0)
            continue;

        uint8_t n = channels[i];
        if (n & 0x40)
            continue;

        _memory->_sfxChannels[i].sfxId = n;
        _memory->_sfxChannels[i].offset = 0.f;
        _memory->_sfxChannels[i].phi = 0.f;
        _memory->_sfxChannels[i].can_loop = false;
        _memory->_sfxChannels[i].is_music = true;
        _memory->_sfxChannels[i].prev_key = 24;
        _memory->_sfxChannels[i].prev_vol = 0.f;
    }
}

void Audio::FillAudioBuffer(void *audioBuffer, size_t offset, size_t size){
    if (audioBuffer == nullptr) {
        return;
    }

    uint32_t *buffer = (uint32_t *)audioBuffer;

    for (size_t i = 0; i < size; ++i){
        int16_t sample = 0;

        for (int c = 0; c < 4; ++c) {
            //bit shifted 3 places to lower volume and avoid clipping
            sample += this->getSampleForChannel(c) >> 3;
        }

        //buffer is stereo, so just send the mono sample to both channels
        buffer[i] = (sample<<16) | (sample & 0xffff);
    }
}

static float key_to_freq(float key)
{
    using std::exp2;
    return 440.f * exp2((key - 33.f) / 12.f);
}

//adapted from zepto8 sfx.cpp (wtfpl license)
int16_t Audio::getSampleForChannel(int channel){
    using std::fabs, std::fmod, std::floor, std::max;

    int const samples_per_second = 22050;

    int16_t sample = 0;

    const int index = _memory->_sfxChannels[channel].sfxId;
 
    // Advance music using the master channel
    if (channel == _memory->_musicChannel.master && _memory->_musicChannel.pattern != -1)
    {
        float const offset_per_second = 22050.f / (183.f * _memory->_musicChannel.speed);
        float const offset_per_sample = offset_per_second / samples_per_second;
        _memory->_musicChannel.offset += offset_per_sample;
        _memory->_musicChannel.volume += _memory->_musicChannel.volume_step / samples_per_second;
        _memory->_musicChannel.volume = std::clamp(_memory->_musicChannel.volume, 0.f, 1.f);

        if (_memory->_musicChannel.volume_step < 0 && _memory->_musicChannel.volume <= 0)
        {
            // Fade out is finished, stop playing the current song
            for (int i = 0; i < 4; ++i) {
                if (_memory->_sfxChannels[i].is_music) {
                    _memory->_sfxChannels[i].sfxId = -1;
                }
            }
            _memory->_musicChannel.pattern = -1;
        }
        else if (_memory->_musicChannel.offset >= 32.f)
        {
            int16_t next_pattern = _memory->_musicChannel.pattern + 1;
            int16_t next_count = _memory->_musicChannel.count + 1;
            //todo: pull out these flags, get memory storage correct as well
            if (_memory->songs[_memory->_musicChannel.pattern].stop) //stop part of the loop flag
            {
                next_pattern = -1;
                next_count = _memory->_musicChannel.count;
            }
            else if (_memory->songs[_memory->_musicChannel.pattern].loop){
                while (--next_pattern > 0 && !_memory->songs[next_pattern].start)
                    ;
            }

            _memory->_musicChannel.count = next_count;
            set_music_pattern(next_pattern);
        }
    }

    if (index < 0 || index > 63) {
        //no (valid) sfx here. return silence
        return 0;
    }

    struct sfx const &sfx = _memory->sfx[index];

    // Speed must be 1—255 otherwise the SFX is invalid
    int const speed = max(1, (int)sfx.speed);

    float const offset = _memory->_sfxChannels[channel].offset;
    float const phi = _memory->_sfxChannels[channel].phi;

    // PICO-8 exports instruments as 22050 Hz WAV files with 183 samples
    // per speed unit per note, so this is how much we should advance
    float const offset_per_second = 22050.f / (183.f * speed);
    float const offset_per_sample = offset_per_second / samples_per_second;
    float next_offset = offset + offset_per_sample;

    // Handle SFX loops. From the documentation: “Looping is turned
    // off when the start index >= end index”.
    float const loop_range = float(sfx.loopRangeEnd - sfx.loopRangeStart);
    if (loop_range > 0.f && next_offset >= sfx.loopRangeStart && _memory->_sfxChannels[channel].can_loop) {
        next_offset = fmod(next_offset - sfx.loopRangeStart, loop_range)
                    + sfx.loopRangeStart;
    }

    int const note_idx = (int)floor(offset);
    int const next_note_idx = (int)floor(next_offset);

    uint8_t key = sfx.notes[note_idx].key;
    float volume = sfx.notes[note_idx].volume / 7.f;
    float freq = key_to_freq(key);

    if (volume == 0.f){
        //volume all the way off. return silence, but make sure to set stuff
        _memory->_sfxChannels[channel].offset = next_offset;

        if (next_offset >= 32.f){
            _memory->_sfxChannels[channel].sfxId = -1;
        }
        else if (next_note_idx != note_idx){
            _memory->_sfxChannels[channel].prev_key = sfx.notes[note_idx].key;
            _memory->_sfxChannels[channel].prev_vol = sfx.notes[note_idx].volume / 7.f;
        }

        return 0;
    }
    
    //TODO: apply effects
    //int const fx = sfx.notes[note_id].effect;

    // Play note
    float waveform = z8::synth::waveform(sfx.notes[note_idx].waveform, phi);

    // Apply master music volume from fade in/out
    // FIXME: check whether this should be done after distortion
    //if (_sfxChannels[chan].is_music) {
    //    volume *= _musicChannel.volume;
    //}

    sample = (int16_t)(32767.99f * volume * waveform);

    // TODO: Apply hardware effects
    //if (m_ram.hw_state.distort & (1 << chan)) {
    //    sample = sample / 0x1000 * 0x1249;
    //}

    _memory->_sfxChannels[channel].phi = phi + freq / samples_per_second;

    _memory->_sfxChannels[channel].offset = next_offset;

    if (next_offset >= 32.f){
        _memory->_sfxChannels[channel].sfxId = -1;
    }
    else if (next_note_idx != note_idx){
        _memory->_sfxChannels[channel].prev_key = sfx.notes[note_idx].key;
        _memory->_sfxChannels[channel].prev_vol = sfx.notes[note_idx].volume / 7.f;
    }

    return sample;
}
