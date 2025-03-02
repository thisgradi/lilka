#pragma once

#include "app.h"

class PlankTimerApp : public App {
private:
    enum ScreenState 
    {
        STATE_INITIAL,  // upon entering or resetting
        STATE_RUNNING,  // upon pressing A (start)
        STATE_ONPAUSE,  // upon pressing A while STATE_RUNNING (pause)
        STATE_FINISH    // upon finishing
    };    
    bool loopContinues;
    ScreenState screen_state;
public:
    PlankTimerApp();

private: 
    void run() override;
    void init();
};