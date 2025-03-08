#pragma once

#include <lilka.h>
#include <string.h>

#include "app.h"
#include "../modplayer/analyzer.h"

typedef struct {
    AudioOutputAnalyzer* analyzer;
    bool isPaused;
    bool isFinished;
    float gain;
    ushort station_ID;
} WebRadioTaskData;

class WebRadioApp : public App {

    enum WindowState {
        WINDOW_MAIN,
        WINDOW_MENU
    };

    enum PlayerView {
        PVIEW_MAIN       = 0,
        PVIEW_VISUALIZER = 1,
        PVIEW_OFF        = 2
    };

private:
    QueueHandle_t playerCommandQueue;
    std::vector<String> v_stations;
    SemaphoreHandle_t webRadioMutex;
    // webRadioTaskData is accessed by both the player task and the app task.
    // It's important to always lock the mutex before accessing it.
    WebRadioTaskData webRadioTaskData = {
        .analyzer = NULL,
        .isPaused = false,
        .isFinished = false,
        .gain = 1.0f,
        .station_ID = 0
    };
    WindowState window_state;
    ushort main_menu_selected_item;
    PlayerView player_view;

public:
    explicit WebRadioApp();
    void run() override;

private:
    void mainWindow();
    void menuWindow();
    void playTask();
    std::vector<String> getStations(String filename);
};
