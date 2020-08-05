
// Crude LED flashing pattern generator.
// Not intended for precise timer.
//
// Version 1.0

#ifndef FLASHER_H
#define FLASHER_H

#include <Arduino.h>

enum class Pattern
{
    Solid,
    OnOff,
    Sin,
    RampUp,
    Flash,
    RandomFlash,
    RandomReverseFlash
};

class flasher
{

private:
    Pattern _pattern;
    int _delay;
    int _maxPwm;
    int _pwmValue = 0;
    bool _repeat = true;
    float _microsPerStep;
    unsigned long _oldMicros;

    byte sinIndex;
    bool toggle = false;
    bool _endOfCycle;

public:
    // Default Constructor
    flasher()
    {
        _pattern = Pattern::Sin;
        _maxPwm = 255;
        _delay = 1000;
    }

    // Constructor.
    // Delay in milliseconds
    flasher(Pattern pattern, int delay, int maxPwm)
    {
        _pattern = pattern;
        _maxPwm = maxPwm;
        _delay = delay;
    }

    inline void setDelay(int delay)
    {
        _delay = delay;
    }

    inline void setPattern(Pattern pattern)
    {
        _pattern = pattern;
    }

    inline void reset()
    {
        _oldMicros = micros();
        sinIndex = 0;
        _pwmValue = 0;
        _endOfCycle = false;
    }

    inline void repeat(bool repeat)
    {
        _repeat = repeat;
    }

    inline bool endOfCycle()
    {
        if (_endOfCycle)
        {
            _endOfCycle = false;
            return true;
        }
        return false;
    }

    inline int getMaxPwm()
    {
        return _maxPwm;
    }

    inline int getPwmValue()
    {

        if (_endOfCycle && !_repeat)
        {
            return 0;
        }

        unsigned long curMicros = micros();

        if ((curMicros - _oldMicros) > _microsPerStep)
        {
            int stepsPassed = (float)(curMicros - _oldMicros) / _microsPerStep;

            if (_pattern == Pattern::Solid)
            {
                //_microsPerStep = (float)_delay * 1000.0;

                _pwmValue = _maxPwm;
            }

            if (_pattern == Pattern::RampUp)
            {
                _microsPerStep = 1.0 / (((float)_maxPwm / (float)_delay) / 1000.0);
                _pwmValue += stepsPassed;

                if (_pwmValue > _maxPwm)
                {
                    _pwmValue = 0;
                    _endOfCycle = true;
                }
            }
            else if (_pattern == Pattern::Sin)
            {
                _microsPerStep = 1.0 / ((180.0 / (float)_delay) / 1000.0);
                sinIndex += stepsPassed;
                if (sinIndex > 180)
                {
                    sinIndex = 0;
                    _endOfCycle = true;
                }
				// TODO: use sin look up table.
                _pwmValue = _maxPwm * sin(radians(sinIndex));
            }
            else if (_pattern == Pattern::OnOff)
            {
                _microsPerStep = ((float)_delay / 2.0) * 1000.0;
                toggle = !toggle;
                _pwmValue = toggle ? _maxPwm : 0;
            }
            else if (_pattern == Pattern::Flash)
            {
                if (toggle)
                {
                    toggle = false;
                    _microsPerStep = ((float)_delay / 10.0) * 1000.0 * 9;
                }
                else
                {
                    toggle = true;
                    _microsPerStep = ((float)_delay / 10.0) * 1000.0;
                }
                _pwmValue = toggle ? _maxPwm : 0;
            }
            else if (_pattern == Pattern::RandomFlash)
            {
                if (toggle)
                {
                    toggle = false;
                    _microsPerStep = ((float)random(_delay / 2, _delay * 1.5)) * 1000.0;
                    _endOfCycle = true;
                }
                else
                {
                    toggle = true;
                    _microsPerStep = 100 * 1000.0;
                }
                _pwmValue = toggle ? _maxPwm : 0;
            }
            else if (_pattern == Pattern::RandomReverseFlash)
            {
                if (toggle)
                {
                    toggle = false;
                    _microsPerStep = ((float)random(_delay / 2, _delay * 1.5)) * 1000.0;
                }
                else
                {
                    toggle = true;
                    _microsPerStep = 100 * 1000.0;
                }
                _pwmValue = toggle ? 0 : _maxPwm;
            }

            _oldMicros = curMicros;
        }

        return _pwmValue;
    }
};

#endif