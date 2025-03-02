#include <string>
#include "plank_timer.h"

PlankTimerApp::PlankTimerApp() : App("PlankTimer")
{
    init();
}

void PlankTimerApp::init()
{
    loopContinues = true;
    screen_state  = STATE_INITIAL;
}

void PlankTimerApp::run()
{
    lilka::Canvas buffer(canvas->width(), canvas->height());
    lilka::State  buttons_state;
    uint16_t      hor_center = canvas->width() / 2;
    const float   PLANK_TIMER_INITIAL = 125; // 2 minutes
    float         plank_timer_full = PLANK_TIMER_INITIAL;
    float         plank_timer = plank_timer_full;  
    float         plank_timer_start;
    
    lilka::display.setFont(FONT_10x20_MONO);

    while (loopContinues)
    {
        buffer.fillScreen(lilka::colors::Black);
        buffer.drawTextAligned("Plank Timer", hor_center, 20, 
                               lilka::Alignment::ALIGN_CENTER, 
                               lilka::Alignment::ALIGN_START);
        buffer.drawTextAligned("Press [B] to exit.", hor_center,
                               canvas->height() - 20,
                               lilka::Alignment::ALIGN_CENTER,
                               lilka::Alignment::ALIGN_START);    

        buttons_state = lilka::controller.getState();
        if (buttons_state.b.pressed) break;

        switch (screen_state)
        {
            case STATE_INITIAL:
            {
                if (buttons_state.a.pressed)
                {
                    plank_timer = plank_timer_full = PLANK_TIMER_INITIAL;
                    plank_timer_start = millis();
                    screen_state = STATE_RUNNING;
                }
                else
                {
                    buffer.drawTextAligned("Press [A] to start.", hor_center,
                                           100,
                                           lilka::Alignment::ALIGN_CENTER,
                                           lilka::Alignment::ALIGN_START);  
                }
                break;
            }
            case STATE_RUNNING:
            {
                if (buttons_state.a.pressed)
                {
                    plank_timer_full = plank_timer;
                    plank_timer_start = millis();
                    screen_state = STATE_ONPAUSE;
                }
                else
                {
                    plank_timer = plank_timer_full - ((millis() - plank_timer_start) / 1000.0f);
                    if (plank_timer > 0.0f)
                    {
                        buffer.drawTextAligned((std::string("Timer: ") + std::to_string(plank_timer)).c_str(), 
                                               hor_center, 90,
                                               lilka::Alignment::ALIGN_CENTER,
                                               lilka::Alignment::ALIGN_START);
                        buffer.drawTextAligned("Press [A] to pause.", hor_center,
                                               110,
                                               lilka::Alignment::ALIGN_CENTER,
                                               lilka::Alignment::ALIGN_START);
                    }
                    else
                    {
                        screen_state = STATE_FINISH;
                    }
                }
                break;
            }
            case STATE_ONPAUSE:
            {
                if (buttons_state.a.pressed)
                {
                    screen_state = STATE_RUNNING;
                }
                else
                {
                    buffer.drawTextAligned((std::string("Timer: ") + std::to_string(plank_timer)).c_str(), 
                                           hor_center, 90,
                                           lilka::Alignment::ALIGN_CENTER,
                                           lilka::Alignment::ALIGN_START);
                    buffer.drawTextAligned("Press [A] to start.", hor_center,
                                           110,
                                           lilka::Alignment::ALIGN_CENTER,
                                           lilka::Alignment::ALIGN_START);  
                }
                break;
            }
            case STATE_FINISH:
            {
                if (buttons_state.a.pressed)
                {
                    screen_state = STATE_INITIAL;
                }
                else
                {
                    buffer.drawTextAligned("Finished!", hor_center,
                                           90,
                                           lilka::Alignment::ALIGN_CENTER,
                                           lilka::Alignment::ALIGN_START); 
                    buffer.drawTextAligned("Press [A] to restart.", hor_center,
                                           110,
                                           lilka::Alignment::ALIGN_CENTER,
                                           lilka::Alignment::ALIGN_START);  
                }
                break;
            }
            default:
                break;
        } 

        canvas->drawCanvas(&buffer);
        queueDraw();
    }
}