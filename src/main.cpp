#include <Arduino.h>
#include <ArduinoJson.h>
#include <DallasTemperature.h>
#include <FS.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <OneWire.h>
#include <algorithm>
#include <vector>

namespace {

constexpr const char* kJsonPath = "/sensor_names.json";
constexpr const char* kJsonTempPath = "/sensor_names.json.tmp";
constexpr gpio_num_t kOneWirePin = GPIO_NUM_2;
constexpr int kSdSpiSckPin = 40;
constexpr int kSdSpiMisoPin = 39;
constexpr int kSdSpiMosiPin = 14;
constexpr int kSdSpiCsPin = 12;
constexpr size_t kMaxNameLength = 24;
constexpr uint32_t kScanIntervalMs = 1500;
constexpr uint32_t kTemperatureIntervalMs = 2000;
constexpr uint32_t kConversionWaitMs = 800;
constexpr int kVisibleRows = 5;
constexpr uint8_t kOfflineScanMissThreshold = 2;
constexpr uint8_t kTempReadFailureThreshold = 2;

struct SensorRecord {
    DeviceAddress address{};
    String addressString;
    String name;
    bool connected = false;
    uint8_t missedScans = 0;
    bool temperatureValid = false;
    float temperatureC = NAN;
    uint8_t tempReadFailures = 0;
};

enum class UiMode {
    Browse,
    EditName,
    ConfirmDelete,
};

OneWire oneWire(kOneWirePin);
DallasTemperature temperatureBus(&oneWire);
std::vector<SensorRecord> sensors;

UiMode uiMode = UiMode::Browse;
size_t selectedIndex = 0;
size_t listScrollOffset = 0;
String editBuffer;
size_t editCursor = 0;
bool deleteConfirmYesSelected = false;

bool sdAvailable = false;
bool conversionPending = false;
bool uiDirty = true;
bool browseLayoutInitialized = false;
bool showInactiveSensors = true;
uint32_t lastScanMs = 0;
uint32_t lastTemperatureRequestMs = 0;
uint32_t conversionStartMs = 0;
String statusLine = "Starting...";

size_t visibleSensorCount() {
    if (showInactiveSensors) {
        return sensors.size();
    }
    return static_cast<size_t>(std::count_if(sensors.begin(), sensors.end(), [](const SensorRecord& sensor) {
        return sensor.connected;
    }));
}

void clampSelectionToVisibleRange() {
    const size_t visibleCount = visibleSensorCount();
    if (visibleCount == 0) {
        selectedIndex = 0;
        listScrollOffset = 0;
        return;
    }

    if (selectedIndex >= visibleCount) {
        selectedIndex = visibleCount - 1;
    }

    if (listScrollOffset >= visibleCount) {
        listScrollOffset = 0;
    }

    if (selectedIndex < listScrollOffset) {
        listScrollOffset = selectedIndex;
    }

    if (selectedIndex >= listScrollOffset + kVisibleRows) {
        listScrollOffset = selectedIndex - kVisibleRows + 1;
    }
}

String addressToString(const uint8_t* address) {
    char buffer[17];
    snprintf(buffer,
             sizeof(buffer),
             "%02X%02X%02X%02X%02X%02X%02X%02X",
             address[0],
             address[1],
             address[2],
             address[3],
             address[4],
             address[5],
             address[6],
             address[7]);
    return String(buffer);
}

bool parseAddressString(const String& text, DeviceAddress outAddress) {
    String compact = text;
    compact.replace("-", "");
    if (compact.length() != 16) {
        return false;
    }

    unsigned int parts[8];
    int parsed = sscanf(compact.c_str(),
                        "%2x%2x%2x%2x%2x%2x%2x%2x",
                        &parts[0],
                        &parts[1],
                        &parts[2],
                        &parts[3],
                        &parts[4],
                        &parts[5],
                        &parts[6],
                        &parts[7]);
    if (parsed != 8) {
        return false;
    }
    for (size_t i = 0; i < 8; ++i) {
        outAddress[i] = static_cast<uint8_t>(parts[i]);
    }
    return true;
}

int findSensorIndexByAddress(const DeviceAddress address) {
    for (size_t i = 0; i < sensors.size(); ++i) {
        if (memcmp(sensors[i].address, address, sizeof(DeviceAddress)) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void sortSensors() {
    std::sort(sensors.begin(), sensors.end(), [](const SensorRecord& a, const SensorRecord& b) {
        if (a.connected != b.connected) {
            return a.connected > b.connected;
        }
        return a.addressString < b.addressString;
    });

    clampSelectionToVisibleRange();
}

void saveMappingsToSd() {
    if (!sdAvailable) {
        statusLine = "SD missing, name not saved";
        return;
    }

    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();

    for (const auto& sensor : sensors) {
        if (sensor.name.isEmpty()) {
            continue;
        }

        JsonObject entry = array.add<JsonObject>();
        entry["address"] = sensor.addressString;
        entry["name"] = sensor.name;
    }

    if (SD.exists(kJsonTempPath)) {
        SD.remove(kJsonTempPath);
    }

    File file = SD.open(kJsonTempPath, FILE_WRITE, true);
    if (!file) {
        statusLine = "Failed to open temp JSON";
        return;
    }

    if (serializeJsonPretty(doc, file) == 0) {
        statusLine = "Failed to write JSON";
        file.close();
        SD.remove(kJsonTempPath);
        return;
    }
    file.close();

    if (SD.exists(kJsonPath) && !SD.remove(kJsonPath)) {
        statusLine = "Failed to replace JSON";
        SD.remove(kJsonTempPath);
        return;
    }

    if (!SD.rename(kJsonTempPath, kJsonPath)) {
        statusLine = "Failed to finalize JSON";
        SD.remove(kJsonTempPath);
        return;
    }

    statusLine = "Name saved to SD";
}

void loadMappingsFromSd() {
    sensors.clear();

    if (!sdAvailable) {
        statusLine = "SD missing";
        return;
    }

    if (!SD.exists(kJsonPath)) {
        statusLine = "JSON not found, new file later";
        return;
    }

    File file = SD.open(kJsonPath, FILE_READ);
    if (!file) {
        statusLine = "Failed to open JSON";
        return;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error || !doc.is<JsonArray>()) {
        statusLine = "Invalid JSON format";
        return;
    }

    for (JsonVariant value : doc.as<JsonArray>()) {
        if (!value.is<JsonObject>()) {
            continue;
        }

        const char* addressText = value["address"] | "";
        const char* nameText = value["name"] | "";

        DeviceAddress address{};
        if (!parseAddressString(String(addressText), address)) {
            continue;
        }

        SensorRecord record;
        memcpy(record.address, address, sizeof(DeviceAddress));
        record.addressString = addressToString(address);
        record.name = String(nameText);
        record.connected = false;
        record.temperatureValid = false;
        record.temperatureC = NAN;
        sensors.push_back(record);
    }

    sortSensors();
    statusLine = "JSON loaded from SD";
}

void scanSensors() {
    std::vector<bool> seen(sensors.size(), false);

    oneWire.reset_search();
    DeviceAddress foundAddress{};

    while (oneWire.search(foundAddress)) {
        if (OneWire::crc8(foundAddress, 7) != foundAddress[7]) {
            continue;
        }

        if (foundAddress[0] != DS18B20MODEL && foundAddress[0] != 0x10 && foundAddress[0] != 0x28) {
            continue;
        }

        int index = findSensorIndexByAddress(foundAddress);
        if (index < 0) {
            SensorRecord record;
            memcpy(record.address, foundAddress, sizeof(DeviceAddress));
            record.addressString = addressToString(foundAddress);
            record.connected = true;
            sensors.push_back(record);
            seen.push_back(true);
        } else {
            SensorRecord& sensor = sensors[static_cast<size_t>(index)];
            sensor.connected = true;
            sensor.missedScans = 0;
            seen[static_cast<size_t>(index)] = true;
        }
    }

    for (size_t i = 0; i < sensors.size(); ++i) {
        if (seen[i]) {
            continue;
        }

        SensorRecord& sensor = sensors[i];
        if (sensor.missedScans < UINT8_MAX) {
            ++sensor.missedScans;
        }

        if (sensor.missedScans >= kOfflineScanMissThreshold) {
            sensor.connected = false;
            sensor.temperatureValid = false;
            sensor.temperatureC = NAN;
            sensor.tempReadFailures = 0;
        }
    }

    sortSensors();
}

void startTemperatureConversion() {
    if (sensors.empty()) {
        return;
    }

    temperatureBus.requestTemperatures();
    conversionPending = true;
    conversionStartMs = millis();
    lastTemperatureRequestMs = conversionStartMs;
}

bool updateTemperaturesIfReady() {
    if (!conversionPending || millis() - conversionStartMs < kConversionWaitMs) {
        return false;
    }

    conversionPending = false;

    for (auto& sensor : sensors) {
        if (!sensor.connected) {
            sensor.temperatureValid = false;
            sensor.temperatureC = NAN;
            sensor.tempReadFailures = 0;
            continue;
        }

        float value = temperatureBus.getTempC(sensor.address);
        if (value == DEVICE_DISCONNECTED_C) {
            if (sensor.tempReadFailures < UINT8_MAX) {
                ++sensor.tempReadFailures;
            }
            if (sensor.tempReadFailures >= kTempReadFailureThreshold) {
                sensor.temperatureValid = false;
                sensor.temperatureC = NAN;
            }
        } else {
            sensor.tempReadFailures = 0;
            sensor.temperatureValid = true;
            sensor.temperatureC = value;
        }
    }

    return true;
}

void ensureSelectionVisible() {
    clampSelectionToVisibleRange();
}

String makeDisplayName(const SensorRecord& sensor) {
    if (!sensor.name.isEmpty()) {
        return sensor.name;
    }
    return "<unnamed>";
}

String makeTemperatureText(const SensorRecord& sensor) {
    if (!sensor.connected || !sensor.temperatureValid) {
        return "NA";
    }
    return String(sensor.temperatureC, 2) + " C";
}

String makeStatusBadgeText() {
    if (uiMode == UiMode::EditName) {
        return "edit";
    }
    if (uiMode == UiMode::ConfirmDelete) {
        return "confirm";
    }

    String lower = statusLine;
    lower.toLowerCase();

    if (lower.indexOf("saved") >= 0) {
        return "saved";
    }
    if (lower.indexOf("sd") >= 0 && (lower.indexOf("fail") >= 0 || lower.indexOf("missing") >= 0)) {
        return "sd!";
    }
    if (lower.indexOf("json") >= 0 && (lower.indexOf("fail") >= 0 || lower.indexOf("invalid") >= 0)) {
        return "json!";
    }
    if (lower.indexOf("start") >= 0) {
        return "init";
    }

    return "";
}

void drawBrowseScreen() {
    if (!browseLayoutInitialized) {
        M5Cardputer.Display.fillScreen(BLACK);
        browseLayoutInitialized = true;
    }

    const int screenWidth = M5Cardputer.Display.width();
    const int screenHeight = M5Cardputer.Display.height();
    const int headerHeight = 20;
    const int rowHeight = 20;
    const int listHeight = rowHeight * kVisibleRows;
    const int addressY = screenHeight - 14;
    constexpr uint16_t kSelectedRowBackground = BLUE;
    constexpr uint16_t kSelectedRowTextColor = WHITE;
    const size_t visibleCount = visibleSensorCount();

    M5Cardputer.Display.fillRect(0, 0, screenWidth, headerHeight, BLACK);
    M5Cardputer.Display.setTextColor(WHITE, BLACK);
    M5Cardputer.Display.setCursor(0, 0);
    M5Cardputer.Display.printf("SD: %s  %s", sdAvailable ? "OK" : "FAIL", showInactiveSensors ? "All" : "Active");

    const String badgeText = makeStatusBadgeText();
    if (!badgeText.isEmpty()) {
        const int badgePaddingX = 4;
        const int badgePaddingY = 2;
        const int textHeight = M5Cardputer.Display.fontHeight();
        const int badgeHeight = textHeight + badgePaddingY * 2;
        const int badgeWidth = M5Cardputer.Display.textWidth(badgeText) + badgePaddingX * 2;
        const int badgeX = screenWidth - badgeWidth - 2;
        const int badgeY = 2;
        const int textY = badgeY + (badgeHeight - textHeight) / 2;
        M5Cardputer.Display.fillRect(badgeX, badgeY, badgeWidth, badgeHeight, DARKGREY);
        M5Cardputer.Display.setTextColor(WHITE, DARKGREY);
        M5Cardputer.Display.setCursor(badgeX + badgePaddingX, textY);
        M5Cardputer.Display.print(badgeText);
    }

    M5Cardputer.Display.fillRect(0, headerHeight, screenWidth, listHeight, BLACK);
    int y = headerHeight;

    auto printRowText = [&](int x, int yPos, const String& text, bool bold) {
        M5Cardputer.Display.setCursor(x, yPos);
        M5Cardputer.Display.print(text);
        if (bold) {
            M5Cardputer.Display.setCursor(x + 1, yPos);
            M5Cardputer.Display.print(text);
        }
    };

    if (visibleCount == 0) {
        M5Cardputer.Display.setCursor(0, y);
        M5Cardputer.Display.print(showInactiveSensors ? "No sensors found yet" : "No active sensors");
    } else {
        size_t end = std::min(visibleCount, listScrollOffset + static_cast<size_t>(kVisibleRows));
        for (size_t i = listScrollOffset; i < end; ++i) {
            const bool selected = (i == selectedIndex);
            const auto& sensor = sensors[i];

            if (selected) {
                M5Cardputer.Display.fillRect(0, y - 1, M5Cardputer.Display.width(), rowHeight - 1, kSelectedRowBackground);
                M5Cardputer.Display.setTextColor(kSelectedRowTextColor, kSelectedRowBackground);
            } else {
                M5Cardputer.Display.setTextColor(WHITE, BLACK);
            }

            String line;
            if (selected && uiMode == UiMode::EditName) {
                String left = editBuffer.substring(0, editCursor);
                String right = editBuffer.substring(editCursor);
                line = ">" + left + "|" + right;
            } else {
                line = String(selected ? ">" : " ") + makeDisplayName(sensor);
            }
            String temp = makeTemperatureText(sensor);
            printRowText(0, y, line, selected);
            printRowText(160, y, temp, selected);
            y += rowHeight;
        }
    }

    M5Cardputer.Display.fillRect(0, addressY, screenWidth, screenHeight - addressY, BLACK);

    if (visibleCount > 0) {
        M5Cardputer.Display.setTextColor(YELLOW, BLACK);
        M5Cardputer.Display.setCursor(0, addressY);
        M5Cardputer.Display.print(sensors[selectedIndex].addressString);
    }

    const String sensorCountText = String("Sensors: ") + String(static_cast<unsigned>(visibleCount)) + " / " +
                                   String(static_cast<unsigned>(sensors.size()));
    const int sensorCountX = screenWidth - M5Cardputer.Display.textWidth(sensorCountText) - 2;
    M5Cardputer.Display.setTextColor(WHITE, BLACK);
    M5Cardputer.Display.setCursor(sensorCountX, addressY);
    M5Cardputer.Display.print(sensorCountText);

    if (uiMode == UiMode::ConfirmDelete) {
        const int dialogWidth = screenWidth - 18;
        const int dialogHeight = 58;
        const int dialogX = (screenWidth - dialogWidth) / 2;
        const int dialogY = (screenHeight - dialogHeight) / 2;

        M5Cardputer.Display.fillRect(dialogX, dialogY, dialogWidth, dialogHeight, DARKGREY);
        M5Cardputer.Display.drawRect(dialogX, dialogY, dialogWidth, dialogHeight, WHITE);

        M5Cardputer.Display.setTextColor(WHITE, DARKGREY);
        M5Cardputer.Display.setCursor(dialogX + 6, dialogY + 8);
        M5Cardputer.Display.print("Delete selected?");

        const int buttonY = dialogY + dialogHeight - 20;
        const int yesX = dialogX + 22;
        const int noX = dialogX + dialogWidth - 42;
        const uint16_t yesBg = deleteConfirmYesSelected ? BLUE : DARKGREY;
        const uint16_t noBg = deleteConfirmYesSelected ? DARKGREY : BLUE;

        M5Cardputer.Display.fillRect(yesX - 4, buttonY - 2, 26, 14, yesBg);
        M5Cardputer.Display.fillRect(noX - 4, buttonY - 2, 20, 14, noBg);

        M5Cardputer.Display.setTextColor(WHITE, yesBg);
        M5Cardputer.Display.setCursor(yesX, buttonY);
        M5Cardputer.Display.print("Yes");

        M5Cardputer.Display.setTextColor(WHITE, noBg);
        M5Cardputer.Display.setCursor(noX, buttonY);
        M5Cardputer.Display.print("No");
    }
}

void renderUi() {
    static UiMode previousMode = uiMode;
    if (previousMode != uiMode) {
        M5Cardputer.Display.fillScreen(BLACK);
        browseLayoutInitialized = false;
    }

    drawBrowseScreen();

    previousMode = uiMode;
    uiDirty = false;
}

void beginEditMode() {
    if (visibleSensorCount() == 0) {
        return;
    }

    uiMode = UiMode::EditName;
    editBuffer = sensors[selectedIndex].name;
    editCursor = editBuffer.length();
    statusLine = "edit";
    uiDirty = true;
}

void saveEditedName() {
    if (visibleSensorCount() == 0) {
        uiMode = UiMode::Browse;
        uiDirty = true;
        return;
    }

    sensors[selectedIndex].name = editBuffer;
    uiMode = UiMode::Browse;
    sortSensors();
    saveMappingsToSd();
    uiDirty = true;
}

void beginDeleteConfirmMode() {
    if (visibleSensorCount() == 0) {
        return;
    }

    uiMode = UiMode::ConfirmDelete;
    deleteConfirmYesSelected = false;
    statusLine = "confirm";
    uiDirty = true;
}

void deleteSelectedSensor() {
    const size_t visibleCount = visibleSensorCount();
    if (visibleCount == 0 || selectedIndex >= sensors.size()) {
        uiMode = UiMode::Browse;
        uiDirty = true;
        return;
    }

    sensors.erase(sensors.begin() + static_cast<std::ptrdiff_t>(selectedIndex));
    clampSelectionToVisibleRange();
    saveMappingsToSd();
    statusLine = "Sensor deleted";
    uiMode = UiMode::Browse;
    uiDirty = true;
}

void cancelDeleteConfirm() {
    uiMode = UiMode::Browse;
    statusLine = "Delete canceled";
    uiDirty = true;
}

void handleDeleteConfirmKeys(const Keyboard_Class::KeysState& keys) {
    for (char c : keys.word) {
        if (c == 27) {
            keys.
            cancelDeleteConfirm();
            return;
        }
        if (c == 'y' || c == 'Y' || c == ';' || c == '.' || c == ',' || c == '/') {
            deleteConfirmYesSelected = true;
            uiDirty = true;
        } else if (c == 'n' || c == 'N') {
            deleteConfirmYesSelected = false;
            uiDirty = true;
        }
    }

    if (keys.enter) {
        if (deleteConfirmYesSelected) {
            deleteSelectedSensor();
        } else {
            cancelDeleteConfirm();
        }
    }
}

void handleBrowseKeys(const Keyboard_Class::KeysState& keys) {
    if (keys.del) {
        beginDeleteConfirmMode();
        return;
    }

    if (keys.fn && keys.word.empty() && !keys.enter && !keys.del && !keys.tab) {
        showInactiveSensors = !showInactiveSensors;
        clampSelectionToVisibleRange();
        statusLine = showInactiveSensors ? "All" : "Active";
        uiDirty = true;
        return;
    }

    const size_t visibleCount = visibleSensorCount();
    bool selectionChanged = false;
    for (char c : keys.word) {
        if (c == ';' && selectedIndex > 0) {
            --selectedIndex;
            ensureSelectionVisible();
            selectionChanged = true;
        } else if (c == '.' && selectedIndex + 1 < visibleCount) {
            ++selectedIndex;
            ensureSelectionVisible();
            selectionChanged = true;
        }
    }

    if (selectionChanged) {
        uiDirty = true;
    }

    if (keys.enter && visibleCount > 0) {
        beginEditMode();
    }
}

void insertCharacterAtCursor(char c) {
    if (!isPrintable(static_cast<unsigned char>(c))) {
        return;
    }
    if (editBuffer.length() >= kMaxNameLength) {
        return;
    }

    editBuffer = editBuffer.substring(0, editCursor) + String(c) + editBuffer.substring(editCursor);
    ++editCursor;
}

void handleEditKeys(const Keyboard_Class::KeysState& keys) {
    bool changed = false;
    if (keys.del && editCursor > 0) {
        editBuffer.remove(editCursor - 1, 1);
        --editCursor;
        changed = true;
    }

    for (char c : keys.word) {
        if (c == ',' && editCursor > 0) {
            --editCursor;
            changed = true;
            continue;
        }
        if (c == '/' && editCursor < editBuffer.length()) {
            ++editCursor;
            changed = true;
            continue;
        }
        if (c == ';' || c == '.') {
            continue;
        }
        size_t lengthBefore = editBuffer.length();
        insertCharacterAtCursor(c);
        if (editBuffer.length() != lengthBefore) {
            changed = true;
        }
    }

    if (keys.enter) {
        saveEditedName();
        return;
    }

    if (changed) {
        uiDirty = true;
    }
}

void initSdCard() {
    SPI.begin(kSdSpiSckPin, kSdSpiMisoPin, kSdSpiMosiPin, kSdSpiCsPin);
    sdAvailable = SD.begin(kSdSpiCsPin, SPI, 25000000);
    if (!sdAvailable) {
        statusLine = "SD init failed";
    }
    uiDirty = true;
}

void setupDevice() {
    auto config = M5.config();
    M5Cardputer.begin(config, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setFont(&fonts::Font2);
}

}  // namespace

void setup() {
    Serial.begin(115200);
    setupDevice();

    initSdCard();
    loadMappingsFromSd();

    temperatureBus.begin();
    temperatureBus.setWaitForConversion(false);

    scanSensors();
    startTemperatureConversion();
    renderUi();
}

void loop() {
    M5Cardputer.update();

    const uint32_t now = millis();

    if (now - lastScanMs >= kScanIntervalMs) {
        scanSensors();
        lastScanMs = now;
        uiDirty = true;
    }

    if (!conversionPending && now - lastTemperatureRequestMs >= kTemperatureIntervalMs) {
        startTemperatureConversion();
    }

    if (updateTemperaturesIfReady()) {
        uiDirty = true;
    }

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState keys = M5Cardputer.Keyboard.keysState();
        if (uiMode == UiMode::Browse) {
            handleBrowseKeys(keys);
        } else if (uiMode == UiMode::EditName) {
            handleEditKeys(keys);
        } else {
            handleDeleteConfirmKeys(keys);
        }
    }

    if (uiDirty) {
        renderUi();
    }
    delay(30);
}
