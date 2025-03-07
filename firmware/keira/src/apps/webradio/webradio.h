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

public:
    explicit WebRadioApp();
    void run() override;

private:
    void mainWindow();
    void playTask();
    std::vector<String> getStations(String filename);
};
