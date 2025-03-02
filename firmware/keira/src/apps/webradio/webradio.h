#pragma once

#include <lilka.h>

#include "app.h"
#include "../modplayer/analyzer.h"

typedef struct {
    AudioOutputAnalyzer* analyzer;
    bool isPaused;
    bool isFinished;
    float gain;
} WebRadioTaskData;

class WebRadioApp : public App {
public:
    explicit WebRadioApp();
    void run() override;

private:
    void mainWindow();
    void playTask();
    QueueHandle_t playerCommandQueue;
    String fileName;

    SemaphoreHandle_t webRadioMutex;
    // webRadioTaskData is accessed by both the player task and the app task.
    // It's important to always lock the mutex before accessing it.
    WebRadioTaskData webRadioTaskData = {
        .analyzer = NULL,
        .isPaused = false,
        .isFinished = false,
        .gain = 1.0f,
    };
};
