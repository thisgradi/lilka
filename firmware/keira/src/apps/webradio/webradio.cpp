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
#include "utils/defer.h"
#include "webradio.h"

#define WEBRADIO_ROOT   "/webradios"

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
                    //lilka::board.disablePowerSavingMode(); this requires hardware modification
                    break;
                }
                case PVIEW_VISUALIZER: player_view = PVIEW_OFF; break;
                case PVIEW_OFF:        player_view = PVIEW_MAIN; break;
            }
        }
    };
}

void WebRadioApp::alert(String title, String message) {
    vTaskDelay(LILKA_UI_UPDATE_DELAY_MS * 100 / portTICK_PERIOD_MS); // to account for button jitter
    lilka::Alert alertDialog(title, message);
    alertDialog.draw(canvas);
    queueDraw();
    while (!alertDialog.isFinished()) {
        alertDialog.update();
    }
}

void WebRadioApp::menuWindow() {
    const int  total_items = 2;

    // canvas->fillScreen(lilka::colors::Black);
    // canvas->setFont(FONT_9x15);
    // canvas->setTextBound(32, 32, canvas->width() - 64, canvas->height() - 32);
    // canvas->setTextColor(lilka::colors::White);
    // canvas->setCursor(32, 32 + 15);
    // canvas->println("Веб-радіо: меню");
    // canvas->println("------------------------");
    // canvas->println("Тицяй B (нижня-права)");
    static String currentPath = WEBRADIO_ROOT;
    const String type_M3U = ".m3u";
    const String type_JSON = ".json";

    lilka::Menu menu("Веб-радіо: меню");
    menu.addItem("Відкрити каталог", 0, 0, "");
    menu.addItem("Додати станцію", 0, 0, "");
    menu.addItem("<< Назад", 0, 0, "");

    menu.addActivationButton(lilka::Button::B); // Back
    menu.addActivationButton(lilka::Button::A); // Enter

    int16_t index = 0;
    while (!menu.isFinished()) {
        menu.update();
        menu.draw(canvas);
        queueDraw();
        index = menu.getCursor();
        if (menu.getButton() == lilka::Button::B) {  // Назад (кнопка B)
            menu.removeActivationButton(lilka::Button::A);
            menu.removeActivationButton(lilka::Button::B);
            window_state = WINDOW_MAIN;
            return;
        }
        switch (index) {
            case 0: {   // Відкрити каталог
                if (!lilka::controller.peekState().a.justPressed) {
                    break;
                }
                vTaskDelay(LILKA_UI_UPDATE_DELAY_MS * 50 / portTICK_PERIOD_MS); // to account for button jitter
                // see LilTrackerApp::filePicker
                if (!SD.exists(currentPath)) { 
                    alert("Помилка", "Не вдалося відкрити директорію " WEBRADIO_ROOT);
                    break;
                }
                File root = SD.open(currentPath);
                if (!root) {
                    alert("Помилка", "Не вдалося відкрити директорію " WEBRADIO_ROOT);
                    break;
                }
                int fileCount = lilka::fileutils.getEntryCount(&SD, currentPath);
                if (0 == fileCount) {
                    alert("Помилка", "Директорія " WEBRADIO_ROOT " порожня");
                    break;
                }
                lilka::Entry entries[fileCount];
                std::vector<String> filenames;
                lilka::fileutils.listDir(&SD, currentPath, entries);

                menu.clearItems();
                menu.setTitle(currentPath);
                for (int i = 0; i < fileCount; ++i) {
                    if (lilka::EntryType::ENT_DIRECTORY == entries[i].type) {
                        // TODO: directories
                        continue;
                    }
                    if (!entries[i].name.endsWith(type_M3U) && !entries[i].name.endsWith(type_JSON)) {
                        continue;
                    }
                    menu.addItem(entries[i].name);
                    filenames.push_back(entries[i].name);
                }
                menu.addItem("<< Назад");

                while (!menu.isFinished()) {
                    menu.update();
                    menu.draw(canvas);
                    queueDraw();
                    int16_t subindex = menu.getCursor();
                    if ((menu.getButton() == lilka::Button::B) || 
                        (menu.getButton() == lilka::Button::A && 
                         (subindex == menu.getItemCount() - 1))) { // Назад
                        menu.clearItems();
                        menu.setTitle("Веб-радіо: меню");
                        menu.addItem("Відкрити каталог", 0, 0, "");
                        menu.addItem("Додати станцію", 0, 0, "");
                        menu.addItem("<< Назад", 0, 0, "");
                        break;
                    }
                    else
                    {
                        if (menu.getButton() == lilka::Button::A)
                        {
                            current_catalog = currentPath + "/" + filenames[subindex];
                            std::vector<String> new_stations = getStations(current_catalog);
                            if (!new_stations.empty()) v_stations = new_stations;
                            menu.clearItems();
                            menu.setTitle("Веб-радіо: меню");
                            menu.addItem("Відкрити каталог", 0, 0, "");
                            menu.addItem("Додати станцію", 0, 0, "");
                            menu.addItem("<< Назад", 0, 0, "");
                            break;
                        }
                    }
                }

                break;
            }
            case 1: {   // Додати станцію

                break;
            }
            case 2: {   // Назад (пункт меню)
                if (menu.getButton() == lilka::Button::A) {
                    menu.removeActivationButton(lilka::Button::A);
                    menu.removeActivationButton(lilka::Button::B);
                    window_state = WINDOW_MAIN;
                    return;
                }
                break;
            }
        }
    }
}

void WebRadioApp::mainWindow() {
    
    // if (PVIEW_OFF == player_view) 
    // {
    //     lilka::board.enablePowerSavingMode(); // this requires hardware modification
    //     return;
    // }

    canvas->fillScreen(lilka::colors::Black);

    xSemaphoreTake(webRadioMutex, portMAX_DELAY);
    bool shouldDrawAnalyzer = (PVIEW_OFF != player_view) && !webRadioTaskData.isPaused && !webRadioTaskData.isFinished;
    xSemaphoreGive(webRadioMutex);    
    if (shouldDrawAnalyzer) {
        //visualizer_DVD_box();
        //visualizer_snake();
        visualizer_gain_graph();
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
                    if (!(webRadioTaskData.isPaused))
                    {
                        if (gen_mp3->isRunning()) gen_mp3->stop();
                        if (httpSource->isOpen()) httpSource->close();

                        if (!(httpSource->open(v_stations[webRadioTaskData.station_ID].c_str())))
                        {
                            alert("Помилка", "Не вдалося відкрити HTTP");
                            httpSource->close();
                            gen_mp3->stop();
                            webRadioTaskData.isPaused = true;
                            xSemaphoreGive(webRadioMutex);
                            break;
                        }    
                        if (!(gen_mp3->begin(httpBufferSource, webRadioTaskData.analyzer)))
                        {
                            alert("Помилка", "Не вдалося запустити MP3");
                            httpSource->close();
                            gen_mp3->stop();
                            webRadioTaskData.isPaused = true;
                            xSemaphoreGive(webRadioMutex);
                            break;
                        }
                    }
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
                    
                    if (gen_mp3->isRunning()) gen_mp3->stop();
                    if (httpSource->isOpen()) httpSource->close();
                    if (!webRadioTaskData.isPaused)
                    {
                        // otherwise change the station
                        //if (httpBufferSource->isOpen()) httpBufferSource->close(); 
                        if (!(httpSource->open(v_stations[webRadioTaskData.station_ID].c_str())))
                        {
                            alert("Помилка", "Не вдалося відкрити HTTP");
                            httpSource->close();
                            gen_mp3->stop();
                            webRadioTaskData.isPaused = true;
                            xSemaphoreGive(webRadioMutex);
                            break;
                        }    
                        if (!(gen_mp3->begin(httpBufferSource, webRadioTaskData.analyzer)))
                        {
                            alert("Помилка", "Не вдалося запустити MP3");
                            httpSource->close();
                            gen_mp3->stop();
                            webRadioTaskData.isPaused = true;
                            xSemaphoreGive(webRadioMutex);
                            break;
                        }
                    }
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

void WebRadioApp::visualizer_DVD_box()
{
    // the movement will not be affected by the gain
    // the hue / opacity / rectangle size could

    static int16_t current_x = 140;
    static int16_t current_y = 120;

    enum DVD_Direction_X { W, E };
    static DVD_Direction_X direction_x = E;

    enum DVD_Direction_Y { N, S };
    static DVD_Direction_Y direction_y = N;

    const int16_t canvas_width = canvas->width();   // 280
    const int16_t canvas_height = canvas->height(); // 240
    const int16_t box_height = 23;
    const int16_t box_width = 37;

    // constexpr int16_t HUE_SPEED_DIV = 4;
    // constexpr int16_t HUE_SCALE = 4;

    // int64_t time = millis();
    // int16_t hue = (time / HUE_SPEED_DIV + 1 / HUE_SCALE) % 360;

    // canvas->fillRect(current_x, current_y, box_width, box_height, lilka::display.color565hsv(hue, 100, 100));
    canvas->drawRect(current_x, current_y, box_width, box_height, lilka::colors::Red);

    if (current_x + box_width == canvas_width) direction_x = W;
    if (current_x == 0) direction_x = E;
    if (current_y == 0) direction_y = N;
    if (current_y + box_height == canvas_height) direction_y = S;

    current_y += (S == direction_y) ? -1 : 1;
    current_x += (W == direction_x) ? -1 : 1;
}

void WebRadioApp::visualizer_snake()
{
    // the movement will not be affected by the gain
    // the hue / opacity could

    struct Coordinates {
        int16_t pixel_x;
        int16_t pixel_y;
    };

    static std::vector<Coordinates> v_snake;

    static int16_t current_x = 140;
    static int16_t current_y = 120;
    
    enum Snake_Direction_X { W, E };
    static Snake_Direction_X direction_x = E;
    
    enum Snake_Direction_Y { N, S };
    static Snake_Direction_Y direction_y = N;
    
    const int16_t snake_max_length = 100;
    const int16_t canvas_width     = canvas->width();  // 280
    const int16_t canvas_height    = canvas->height(); // 240

    int16_t snake_actual_length = v_snake.size();
    
    if (current_x == canvas_width)  direction_x = W;
    if (current_x == 0)             direction_x = E;
    if (current_y == 0)             direction_y = N;
    if (current_y == canvas_height) direction_y = S;
    
    current_y += (S == direction_y) ? -1 : 1;
    current_x += (W == direction_x) ? -1 : 1;

    if (snake_max_length == snake_actual_length) v_snake.erase(v_snake.begin());
    v_snake.push_back({current_x, current_y});

    for (int i = 0; i < snake_actual_length; ++i)
    {
        canvas->drawPixel(v_snake[i].pixel_x, v_snake[i].pixel_y, lilka::colors::Red);
    }
}

void WebRadioApp::visualizer_gain_graph()
{
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

std::vector<String> WebRadioApp::getStations(String filename)
{
    static bool b_once = true;
    std::vector<String> stations;
    if (b_once) 
    {
        b_once = false;
        stations.push_back("http://stream.srg-ssr.ch/m/rsj/mp3_128"); // Radio Swiss Jazz (audio/mpeg)
        stations.push_back("http://31.128.79.192:8000/live"); // yedenradio.com (audio/mpeg)
        stations.push_back("http://87.118.87.46:8888/stream/1/"); // radiosuperoldie.com
        return stations;
    }
    stations.clear();

    lilka::serial_log("getStations filename: [%s]\n", filename.c_str());
    File file = SD.open(filename, FILE_READ);
    if (!file) {
        alert("Помилка", "Не вдалося відкрити файл " + filename);
        return v_stations;
    }
    Defer closeFile([&file]() { file.close(); });
    lilka::serial_log("file.size(): [%d]\n", file.size());
    uint8_t* buff = new uint8_t[file.size()];
    std::unique_ptr<uint8_t[]> buffPtr(buff);
    file.read(buff, file.size());
    
    // In .M3U stations look as follows:
    //#EXTINF: ... , sample name - NNN kbit/sLF
    //http://sample.TLD/foobarLF
    //#EXTINF: ... , sample name - NNN kbit/sLF
    //http://sample.TLD/foobarLF
    
    size_t file_size = file.size();
    size_t i = 0;
    bool url_is_station = false;
    while (file_size > i)
    {
        if (0 == memcmp((char*)(buff + (i * sizeof(uint8_t))), "http:", 5))
        {
            size_t url_length = 5;
            if (url_is_station || i == 0)
            {            
                while ('\n' != buff[i + url_length]) ++url_length;
                uint8_t url_buff[url_length + 1] = "";
                url_buff[url_length] = '\0';
                memcpy(url_buff, (char*)(buff + i * sizeof(uint8_t)), url_length);

                url_is_station = false;
                stations.push_back((char*)url_buff);
                lilka::serial_log("url: [%s]\n", (char*)url_buff);
            }
            i += url_length + 1;
            continue;
        }
        else
        {
            url_is_station = false; // url will be considered a station only if it starts right after "\n"
        }
        if ('#' == (char)buff[i]) 
        {
            if (0 == memcmp((char*)(buff + i * sizeof(uint8_t)), "#EXTINF:", 8))
            {
                // lilka::serial_log("#EXTINF:\n");
                i += 8;
                continue;
            }
        }
        if ('\n' == (char)buff[i]) 
        {
            // lilka::serial_log("LF");
            url_is_station = true; // if we find a line starting with "http:" next, it will be considered a station
        }
        ++i;
    }
    
    alert("Оброблено!", String("Станцій: ") + stations.size());
    return stations;
}