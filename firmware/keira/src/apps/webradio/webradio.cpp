// WebRadio - перемагач MP3 (воно-ж MPEG) потоків.
// Створено на базі ModPlayer від "Alder" та "and3rson".
// POC by BFB Workshop
//
// > ModPlayer – програвач MOD-файлів на базі бібліотеки ESP8266Audio для Lilka зі звуковим модулем PCM5102.
// > Автори: Олексій "Alder" Деркач (https://github.com/alder) та Андрій "and3rson" Дунай (https://github.com/and3rson)
// > Детальніше про формат та його історію — https://en.wikipedia.org/wiki/MOD_(file_format)
// > Найбільший архів з музикою у форматі MOD – https://modarchive.org/
//
// Актуальні баги:
// * Зависання відтворення в момент старту з зависанням графіку;
// * Зависання відтворення через тривалий час з порожнім графіком (та, подекуди, шумом);
// * Відтворення одних потоків, й неможливість відтворення інших з типом audio/mpeg;
//
// Пошук станції:
// 1. Знайшли веб-сторінку з кнопочкою "Play";
// 2. ПКМ -> Inspect або F12;
// 3. Ctrl+F -> audio/mpeg -> копіюємо значення "src" з лапками включно;
//

#include <AudioOutputI2S.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourcePROGMEM.h>
#include <AudioOutputI2SNoDAC.h>
#include <AudioFileSourceHTTPStream.h>
#include <AudioGeneratorMP3.h>

#include "webradio.h"

typedef enum {
    CMD_SET_PAUSED,
    CMD_SET_GAIN,
    CMD_STOP,
} PlayerCommandType;

typedef struct {
    PlayerCommandType type;
    union {
        bool isPaused;
        float gain;
    };
} PlayerCommand;

WebRadioApp::WebRadioApp() :
    App("WebRadio", 0, 0, lilka::display.width(), lilka::display.height()),
    playerCommandQueue(xQueueCreate(8, sizeof(PlayerCommand))) {
    setFlags(AppFlags::APP_FLAG_FULLSCREEN);
    // This app will run on core 1 since it's already more busy with drawing Keira stuff.
    // However, player task will run on core 0 since it's less busy. /AD
    setCore(1);
    // Get local file name (path minus mount point)
    fileName = "http://31.128.79.192:8000/live"; // yedenradio.com (audio/mpeg)
    //fileName = "https://online.radioroks.ua/RadioROKS"; // radioroks.ua (audio/mpeg) doesn't work somehow
    webRadioMutex = xSemaphoreCreateMutex();
    xSemaphoreGive(webRadioMutex);
}

void WebRadioApp::run() {
    // Start the player task on core 0
    if (xTaskCreatePinnedToCore(
        [](void* arg) {
            WebRadioApp* app = static_cast<WebRadioApp*>(arg);
            app->playTask();
        },
        "WebRadio",
        32768,
        this,
        1,
        nullptr,
        0
    ) != pdPASS)
    {
        lilka::serial_log("Failed to create webradio task");
        xSemaphoreGive(webRadioMutex);
    };

    while (1) {
        mainWindow();
        xSemaphoreTake(webRadioMutex, portMAX_DELAY);
        WebRadioTaskData info = webRadioTaskData;
        xSemaphoreGive(webRadioMutex);
        lilka::State state = lilka::controller.getState();
        if (state.a.justPressed) {
            PlayerCommand command = {.type = CMD_SET_PAUSED, .isPaused = !info.isPaused};
            xQueueSend(playerCommandQueue, &command, portMAX_DELAY);
        };
        if (state.b.justPressed) {
            // Exit app
            PlayerCommand command = {.type = CMD_STOP};
            xQueueSend(playerCommandQueue, &command, portMAX_DELAY);
            break;
        };
        if (state.up.justPressed) {
            PlayerCommand command = {.type = CMD_SET_GAIN, .gain = info.gain + 0.25f};
            xQueueSend(playerCommandQueue, &command, portMAX_DELAY);
        };
        if (state.down.justPressed) {
            PlayerCommand command = {.type = CMD_SET_GAIN, .gain = info.gain - 0.25f};
            xQueueSend(playerCommandQueue, &command, portMAX_DELAY);
        };
    };
}

void WebRadioApp::mainWindow() {
    canvas->fillScreen(lilka::colors::Black);

    xSemaphoreTake(webRadioMutex, portMAX_DELAY);
    bool shouldDrawAnalyzer = !webRadioTaskData.isPaused && !webRadioTaskData.isFinished;
    xSemaphoreGive(webRadioMutex);

    if (shouldDrawAnalyzer) {
        int16_t analyzerBuffer[ANALYZER_BUFFER_SIZE];
        xSemaphoreTake(webRadioMutex, portMAX_DELAY);
        webRadioTaskData.analyzer->readBuffer(analyzerBuffer);
        int16_t head = webRadioTaskData.analyzer->getBufferHead();
        float gain = webRadioTaskData.gain;
        xSemaphoreGive(webRadioMutex);

        int16_t prevX, prevY;
        int16_t width = canvas->width();
        int16_t height = canvas->height();

        constexpr int16_t HUE_SPEED_DIV = 4;
        constexpr int16_t HUE_SCALE = 4;
        int16_t yCenter = height * 5 / 7;

        int64_t time = millis();

        for (int i = 0; i < ANALYZER_BUFFER_SIZE; i += 4) {
            int x = i * width / ANALYZER_BUFFER_SIZE;
            int index = (i + head) % ANALYZER_BUFFER_SIZE;
            float amplitude = static_cast<float>(analyzerBuffer[index]) / 32768 * fmaxf(gain, 1.0f);
            int y = yCenter + static_cast<int>(amplitude * height / 2);
            if (i > 0) {
                int16_t hue = (time / HUE_SPEED_DIV + i / HUE_SCALE) % 360;
                canvas->drawLine(prevX, prevY, x, y, lilka::display.color565hsv(hue, 100, 100));
            }
            prevX = x;
            prevY = y;
        }
    }

    xSemaphoreTake(webRadioMutex, portMAX_DELAY);
    // Copy webRadioTaskData to prevent blocking the mutex for too long
    WebRadioTaskData info = this->webRadioTaskData;
    xSemaphoreGive(webRadioMutex);

    canvas->setFont(FONT_9x15);
    canvas->setTextBound(32, 32, canvas->width() - 64, canvas->height() - 32);
    canvas->setTextColor(lilka::colors::White);
    canvas->setCursor(32, 32 + 15);
    canvas->println("Веб-радіо");
    canvas->println("------------------------");
    canvas->println("A - Відтворення / пауза");
    canvas->setFont(FONT_9x15_SYMBOLS);
    canvas->print("↑ / ↓");
    canvas->setFont(FONT_9x15);
    canvas->println(" - Гучність");
    canvas->println("B - Вихід");
    canvas->println("------------------------");
    canvas->println("Гучність: " + String(info.gain));
    if (info.isFinished) canvas->println("Стрім закінчився");
    if (info.isPaused) canvas->setTextColor(lilka::colors::Black_shadows);
    canvas->printf("\n%s", fileName.c_str());
    
    queueDraw();
}

void WebRadioApp::playTask() {
    // Source/sink order:
    // modSource -> modBufferSource -> mod -> analyzer -> out
    
    // Create output
    AudioOutputI2S* out = new AudioOutputI2S();
    std::unique_ptr<AudioOutputI2S> outPtr(out); // Auto-delete on task return
    out->SetPinout(LILKA_I2S_BCLK, LILKA_I2S_LRCK, LILKA_I2S_DOUT);
    
    // Set initial volume
    float initialGain = (1.0f * lilka::audio.getVolume() / 100);
    out->SetGain(initialGain);
    webRadioTaskData.gain = initialGain;

    // Create output analyzer
    webRadioTaskData.analyzer = new AudioOutputAnalyzer(out);
    std::unique_ptr<AudioOutputAnalyzer> analyzerPtr(webRadioTaskData.analyzer);
    
    // Create source
    AudioFileSourceHTTPStream* httpSource = new AudioFileSourceHTTPStream(fileName.c_str());
    std::unique_ptr<AudioFileSource> httpSourcePtr(httpSource);
    
    // Create buffer
    AudioFileSourceBuffer* httpBufferSource = new AudioFileSourceBuffer(httpSource, 4096);

    // Create generator
    AudioGeneratorMP3* gen_mp3 = new AudioGeneratorMP3();
    std::unique_ptr<AudioGeneratorMP3> mp3Ptr(gen_mp3);

    // Start generator
    gen_mp3->begin(httpBufferSource, webRadioTaskData.analyzer);
    
    xSemaphoreTake(webRadioMutex, portMAX_DELAY);
    webRadioTaskData.isPaused = false;
    webRadioTaskData.isFinished = false;
    xSemaphoreGive(webRadioMutex);
        
    while (1) { // output the chunk while monitoring the commands from core 1
        PlayerCommand command;
        if (xQueueReceive(playerCommandQueue, &command, 0) == pdTRUE) {
            switch (command.type) {
                case CMD_SET_PAUSED:
                    xSemaphoreTake(webRadioMutex, portMAX_DELAY);
                    webRadioTaskData.isPaused = command.isPaused;
                    xSemaphoreGive(webRadioMutex);
                    break;
                case CMD_SET_GAIN:
                    xSemaphoreTake(webRadioMutex, portMAX_DELAY);
                    webRadioTaskData.gain = command.gain;
                    xSemaphoreGive(webRadioMutex);
                    if (webRadioTaskData.gain < 0) webRadioTaskData.gain = 0;
                    if (webRadioTaskData.gain > 4) webRadioTaskData.gain = 4;
                    out->SetGain(webRadioTaskData.gain);
                    break;
                case CMD_STOP:
                    xSemaphoreTake(webRadioMutex, portMAX_DELAY);
                    webRadioTaskData.isFinished = true;
                    xSemaphoreGive(webRadioMutex);
                    break;
            }
        }
        xSemaphoreTake(webRadioMutex, portMAX_DELAY);
        if (!webRadioTaskData.isPaused) {               // while playing
            if (!gen_mp3->loop()) {                     // check if the chunk is finished
            //if (!gen_mp3->isRunning()) {
                //gen_mp3->stop();
                xSemaphoreGive(webRadioMutex);
                break;                                  // if finished -> go to the next chunk
            }
        }
        xSemaphoreGive(webRadioMutex);
        taskYIELD(); // Give app a chance to acquire webRadioMutex
    }

    // Tasks must ALWAYS delete themselves before exiting, or we're get IllegalInstruction panic
    vTaskDelete(NULL);
}
