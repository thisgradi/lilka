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
// або
// Шукайте http посилання в загальнодоступних .m3u списках відтворення: 
// Приклад: https://github.com/junguler/m3u-radio-music-playlists
//

#include <AudioOutputI2S.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourcePROGMEM.h>
#include <AudioOutputI2SNoDAC.h>
#include <AudioFileSourceHTTPStream.h>
#include <AudioGeneratorMP3.h>
//#include "lilka/board.h"

#include "webradio.h"

typedef enum {
    CMD_SET_PAUSED,
    CMD_SET_GAIN,
    CMD_CHANGE_STATION,
    CMD_STOP
} PlayerCommandType;

typedef enum {
    SSDIR_NEXT,
    SSDIR_PREV
} StationScrollDirection;

typedef struct {
    PlayerCommandType type;
    union {
        bool isPaused;
        float gain;
        bool direction;
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
    v_stations = getStations("foobar");
    window_state = WINDOW_MAIN;
    player_view = PVIEW_MAIN;
    main_menu_selected_item = 0;

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
        switch (window_state)
        {
            case WINDOW_MAIN:
                mainWindow();
                break;
            case WINDOW_MENU:
                menuWindow();
                break;
            default:
                mainWindow();
                break;
        }
        
        xSemaphoreTake(webRadioMutex, portMAX_DELAY);
        WebRadioTaskData info = webRadioTaskData;
        xSemaphoreGive(webRadioMutex);
        lilka::State state = lilka::controller.getState();
        if (state.a.justPressed) {
            // Play / Pause
            PlayerCommand command = {.type = CMD_SET_PAUSED, .isPaused = !info.isPaused};
            xQueueSend(playerCommandQueue, &command, portMAX_DELAY);
        };
        if (state.b.justPressed) {
            if (WINDOW_MENU == window_state) 
            {
                // Return from menu
                window_state = WINDOW_MAIN;
                // Play
                PlayerCommand command = {.type = CMD_SET_PAUSED, .isPaused = false};
                xQueueSend(playerCommandQueue, &command, portMAX_DELAY);
            }
            else
            {
                // Exit app
                PlayerCommand command = {.type = CMD_STOP};
                xQueueSend(playerCommandQueue, &command, portMAX_DELAY);
                break;
            }
        };
        if (state.up.justPressed) {
            // Volume up
            PlayerCommand command = {.type = CMD_SET_GAIN, .gain = info.gain + 0.05f};
            xQueueSend(playerCommandQueue, &command, portMAX_DELAY);
        };
        if (state.down.justPressed) {
            // Volume down
            PlayerCommand command = {.type = CMD_SET_GAIN, .gain = info.gain - 0.05f};
            xQueueSend(playerCommandQueue, &command, portMAX_DELAY);
        };
        if (state.right.justPressed) {
            // Station next
            PlayerCommand command = {.type = CMD_CHANGE_STATION, .direction = SSDIR_NEXT};
            xQueueSend(playerCommandQueue, &command, portMAX_DELAY);
        };
        if (state.left.justPressed) {
            // Station previous
            PlayerCommand command = {.type = CMD_CHANGE_STATION, .direction = SSDIR_PREV};
            xQueueSend(playerCommandQueue, &command, portMAX_DELAY);
        };
        if (state.c.justPressed) {
            // Open menu (set core 0 on pause, draw the menu with core 1)
            PlayerCommand command = {.type = CMD_SET_PAUSED, .isPaused = true};
            xQueueSend(playerCommandQueue, &command, portMAX_DELAY);
            window_state = WINDOW_MENU;
        };
        if (state.d.justPressed) {
            // Change view
            switch (player_view)
            {
                case PVIEW_MAIN:       
                {
                    player_view = PVIEW_VISUALIZER; 
                    //lilka::board.disablePowerSavingMode(); this doesn't work yet, audio and screen are hardwired 
                    break;
                }
                case PVIEW_VISUALIZER: player_view = PVIEW_OFF; break;
                case PVIEW_OFF:        player_view = PVIEW_MAIN; break;
            }
        }
    };
}

void WebRadioApp::menuWindow() {
    const int  total_items = 2;

    canvas->fillScreen(lilka::colors::Black);
    canvas->setFont(FONT_9x15);
    canvas->setTextBound(32, 32, canvas->width() - 64, canvas->height() - 32);
    canvas->setTextColor(lilka::colors::White);
    canvas->setCursor(32, 32 + 15);
    canvas->println("Веб-радіо: меню");
    canvas->println("------------------------");

    queueDraw();
}

void WebRadioApp::mainWindow() {
    
    // if (PVIEW_OFF == player_view) 
    // {
    //     lilka::board.enablePowerSavingMode(); // this doesn't work yet, audio and screen are hardwired 
    //     return;
    // }

    canvas->fillScreen(lilka::colors::Black);

    xSemaphoreTake(webRadioMutex, portMAX_DELAY);
    bool shouldDrawAnalyzer = (PVIEW_OFF != player_view) && !webRadioTaskData.isPaused && !webRadioTaskData.isFinished;
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
        int16_t yCenter = height;
        if (PVIEW_MAIN == player_view) yCenter *= 0.71;
        else                           yCenter *= 0.5;

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

    if (PVIEW_MAIN == player_view)
    {
        xSemaphoreTake(webRadioMutex, portMAX_DELAY);
        // Copy webRadioTaskData to prevent blocking the mutex for too long
        WebRadioTaskData info = this->webRadioTaskData;
        xSemaphoreGive(webRadioMutex);

        canvas->setFont(FONT_9x15);
        canvas->setTextBound(32, 32, canvas->width() - 64, canvas->height() - 32);
        canvas->setTextColor(lilka::colors::White);
        canvas->setCursor(32, 32 + 15);
        canvas->println("Веб-радіо: плеєр        ");
        canvas->println("------------------------");
        canvas->println("  гучніше       меню   ");
        canvas->print(  "назад  далі  вікно ");
        if (info.isPaused) canvas->println("грати"); 
        else               canvas->println("пауза");
        canvas->println("  тихіше        вийти  ");
        canvas->println("------------------------");
        canvas->println("Гучність: " + String(info.gain));
        if (info.isFinished) canvas->println("Стрім закінчився");
        if (info.isPaused) canvas->setTextColor(lilka::colors::Black_shadows);
        xSemaphoreTake(webRadioMutex, portMAX_DELAY);
        canvas->printf("\n%s", v_stations[webRadioTaskData.station_ID].c_str());
        xSemaphoreGive(webRadioMutex);
    }

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
    AudioFileSourceHTTPStream* httpSource = new AudioFileSourceHTTPStream(v_stations[webRadioTaskData.station_ID].c_str());
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
                    if (webRadioTaskData.gain < 0) webRadioTaskData.gain = 0;
                    if (webRadioTaskData.gain > 4) webRadioTaskData.gain = 4;
                    xSemaphoreGive(webRadioMutex);
                    out->SetGain(webRadioTaskData.gain);
                    break;
                case CMD_CHANGE_STATION:
                    xSemaphoreTake(webRadioMutex, portMAX_DELAY);
                    if ((command.direction == SSDIR_PREV)
                    &&  (webRadioTaskData.station_ID > 0))
                        --webRadioTaskData.station_ID;
                    else 
                    if ((command.direction == SSDIR_NEXT)
                    &&  (webRadioTaskData.station_ID < (v_stations.size() - 1)))
                        ++webRadioTaskData.station_ID;
                    else 
                    {
                        xSemaphoreGive(webRadioMutex);
                        break; // station can't be changed
                    }

                    // otherwise change the station
                    if (gen_mp3->isRunning()) gen_mp3->stop();
                    //if (httpBufferSource->isOpen()) httpBufferSource->close(); 
                    if (httpSource->isOpen()) httpSource->close();
                    httpSource->open(v_stations[webRadioTaskData.station_ID].c_str());
                    gen_mp3->begin(httpBufferSource, webRadioTaskData.analyzer);
                    xSemaphoreGive(webRadioMutex);
                    break;
                case CMD_STOP:
                    xSemaphoreTake(webRadioMutex, portMAX_DELAY);
                    gen_mp3->stop();
                    webRadioTaskData.isFinished = true;
                    xSemaphoreGive(webRadioMutex);
                    break;
            }
        }
        xSemaphoreTake(webRadioMutex, portMAX_DELAY);
        if (webRadioTaskData.isFinished) {
            gen_mp3->stop();
            xSemaphoreGive(webRadioMutex);
            break;
        }
        if (!webRadioTaskData.isPaused) {               // while playing
            if (!gen_mp3->loop()) {                     // check if the chunk is finished
                gen_mp3->stop();
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

std::vector<String> WebRadioApp::getStations(String filename)
{
    std::vector<String> v_stations;
    v_stations.push_back("http://stream.srg-ssr.ch/m/rsj/mp3_128"); // Radio Swiss Jazz (audio/mpeg)
    v_stations.push_back("http://31.128.79.192:8000/live"); // yedenradio.com (audio/mpeg)
    v_stations.push_back("http://87.118.87.46:8888/stream/1/"); // radiosuperoldie.com
    return v_stations;
}