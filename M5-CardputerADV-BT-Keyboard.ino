/*Teno 2026 - M5 Cardputer ADV - 
Me volví loco buscando un firmware que convierta a la M5 Cardputer ADV en un teclado Bluetooth y que funcionen las flechas de dirección, F1 a F12, insert, etc...
Como creo que no existe, lo hice.
Si usted tiene un firmware que haga todo esto y encima sea USB o Bluetooth, dicho código será digno de que yo lo vanaglorie y diga "Esto si que no es chicharron de laucha"

Notas de instalación:
  - Instalar "NimBLE-Arduino" desde el Library Manager de Arduino IDE
  - Board: M5Stack-CardputerS3 (board support oficial de M5Stack)
  - Partition Scheme: Huge APP (3MB No OTA) — NimBLE necesita espacio
*/

#include "M5Cardputer.h"
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEHIDDevice.h>
#include <HIDTypes.h>

// Descriptor HID
// Define la estructura del reporte que el host espera recibir
// Está probado solo en Linux y Android, así que no sé si anda en todo como debería
// La forma de detección de shift y fn que puse no son elegantes pero son muy veloces
static uint8_t hidReportDescriptor[] = {
    USAGE_PAGE(1),        0x01,  // Generic Desktop
    USAGE(1),             0x06,  // Keyboard
    COLLECTION(1),        0x01,  // Application
      // Byte 0: Modifier keys (8 bits: CTRL, SHIFT, ALT, GUI x2)
      USAGE_PAGE(1),      0x07,
      USAGE_MINIMUM(1),   0xE0,
      USAGE_MAXIMUM(1),   0xE7,
      LOGICAL_MINIMUM(1), 0x00,
      LOGICAL_MAXIMUM(1), 0x01,
      REPORT_SIZE(1),     0x01,
      REPORT_COUNT(1),    0x08,
      HIDINPUT(1),        0x02,  // Data, Variable, Absolute
      // Byte 1: Reservado
      REPORT_COUNT(1),    0x01,
      REPORT_SIZE(1),     0x08,
      HIDINPUT(1),        0x03,  // Constant
      // Byte 2: LEDs (NumLock, CapsLock, ScrollLock, etc.)
      USAGE_PAGE(1),      0x08,
      USAGE_MINIMUM(1),   0x01,
      USAGE_MAXIMUM(1),   0x05,
      REPORT_COUNT(1),    0x05,
      REPORT_SIZE(1),     0x01,
      HIDOUTPUT(1),       0x02,
      REPORT_COUNT(1),    0x01,
      REPORT_SIZE(1),     0x03,
      HIDOUTPUT(1),       0x03,
      // Bytes 3-8: Keycodes (hasta 6 teclas simultáneas)
      USAGE_PAGE(1),      0x07,
      USAGE_MINIMUM(1),   0x00,
      USAGE_MAXIMUM(1),   0x65,
      LOGICAL_MINIMUM(1), 0x00,
      LOGICAL_MAXIMUM(1), 0x65,
      REPORT_COUNT(1),    0x06,
      REPORT_SIZE(1),     0x08,
      HIDINPUT(1),        0x00,  // Data, Array, Absolute
    END_COLLECTION(0)
};

//Variables globales de BLE
NimBLEServer*         pServer = nullptr;
NimBLEHIDDevice*      pHID    = nullptr;
NimBLECharacteristic* pInput  = nullptr;  // Canal de envío: teclado → host
bool bleConnected = false;

//Sstructura del reporte HID (equivalente al KeyReport de la versión USB https://github.com/TenoTrash/M5-CardputerADV-USB-Keyboard)
struct KeyReport {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[6];
};

// Último reporte enviado (para evitar enviar repetidos)
KeyReport lastReport = {0};

// Último estado dibujado en pantalla (para no redibujar si no cambió nada)
bool    lastDisplayFn        = false;
uint8_t lastDisplayModifiers = 0xFF;  // 0xFF fuerza el primer dibujado

//Posiciones en pantalla - alguien tiene ganas de ponerle iconitos y dibujitos?
#define Y_TITLE   5
#define Y_HELP   40
#define Y_STATUS 115

//Forward declarations (necesarias porque ServerCallbacks llama a drawUI - previene el parpadeo del display)
void drawUI();
void drawStatus(bool fn, uint8_t modifiers);

//Callbacks de conexión/desconexión BLE
//Nota: NimBLE >= 1.4 usa NimBLEConnInfo en lugar de solo NimBLEServer*
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pSvr, NimBLEConnInfo& connInfo) override {
        bleConnected = true;
        drawUI();  // Actualiza el indicador de conexión en pantalla
    }
    void onDisconnect(NimBLEServer* pSvr, NimBLEConnInfo& connInfo, int reason) override {
        bleConnected = false;
        drawUI();
        // Vuelve a anunciar para permitir reconexión
        NimBLEDevice::startAdvertising();
    }
};

//Dibuja la interfaz fija (título + ayuda + estado BLE)
void drawUI() {
    M5Cardputer.Display.clear();

    // Título + indicador BLE en la misma línea (se puede poner un ícono! acepto PRs)
    M5Cardputer.Display.setTextSize(1.8);
    M5Cardputer.Display.setTextColor(GREEN);
    M5Cardputer.Display.setCursor(10, Y_TITLE);
    M5Cardputer.Display.print("Teno BLE KB");
    M5Cardputer.Display.setTextColor(bleConnected ? GREEN : YELLOW);
    M5Cardputer.Display.print(bleConnected ? " [OK]" : " [..]");

    // Ayuda (tamaño grande para que se lea bien)
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setTextColor(WHITE);

    M5Cardputer.Display.setCursor(10, Y_HELP);
    M5Cardputer.Display.println("F1-F12 : FN+1..=");

    M5Cardputer.Display.setCursor(10, Y_HELP + 25);
    M5Cardputer.Display.println("INS    : FN+SPC");

    M5Cardputer.Display.setCursor(10, Y_HELP + 50);
    M5Cardputer.Display.setTextSize(1.5);
    M5Cardputer.Display.println("ARROWS : FN+; , . /");

    // Forzar redibujado del status en el próximo loop
    lastDisplayModifiers = 0xFF;
}

//Dibuja el estado de modificadores (CTRL, SHIFT, ALT, FN)
void drawStatus(bool fn, uint8_t modifiers) {

    // Solo redibujar si algo cambió respecto a lo que ya está en pantalla
    if (fn == lastDisplayFn && modifiers == lastDisplayModifiers) return;

    lastDisplayFn        = fn;
    lastDisplayModifiers = modifiers;

    // Limpiar solo la franja de status antes de redibujar (evita ghosting)
    M5Cardputer.Display.fillRect(0, Y_STATUS, M5Cardputer.Display.width(), 20, BLACK);

    int x = 10;

    // CTRL (bit 0 del campo modifiers)
    M5Cardputer.Display.setTextSize(1.5);
    M5Cardputer.Display.setTextColor((modifiers & 0x01) ? RED : DARKGREY);
    M5Cardputer.Display.setCursor(x, Y_STATUS);
    M5Cardputer.Display.print("CTRL ");
    x += 45;

    // SHIFT (bit 1)
    M5Cardputer.Display.setTextSize(1.5);
    M5Cardputer.Display.setTextColor((modifiers & 0x02) ? GREEN : DARKGREY);
    M5Cardputer.Display.setCursor(x, Y_STATUS);
    M5Cardputer.Display.print("SHIFT ");
    x += 55;

    // ALT (bit 2)
    M5Cardputer.Display.setTextSize(1.5);
    M5Cardputer.Display.setTextColor((modifiers & 0x04) ? YELLOW : DARKGREY);
    M5Cardputer.Display.setCursor(x, Y_STATUS);
    M5Cardputer.Display.print("ALT ");
    x += 45;

    // FN no es parte del HID, lo mostramos aparte
    M5Cardputer.Display.setTextSize(1.5);
    M5Cardputer.Display.setTextColor(fn ? CYAN : DARKGREY);
    M5Cardputer.Display.setCursor(x, Y_STATUS);
    M5Cardputer.Display.print("FN");
}

//Setup
void setup() {
    // Configuración base del equipo
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    // Rotación de pantalla
    M5Cardputer.Display.setRotation(1);

    // Dibujar UI fija una sola vez
    drawUI();

    //Inicializar NimBLE
    NimBLEDevice::init("Teno BLE Keyboard");

    // Habilitar bonding: el host recuerda el teclado entre sesiones
    NimBLEDevice::setSecurityAuth(BLE_SM_PAIR_AUTHREQ_BOND);

    // Crear servidor BLE y asignar callbacks de conexión
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    //Crear dispositivo HID
    pHID = new NimBLEHIDDevice(pServer);

    //API NimBLE >= 1.4: métodos con prefijo set/get
    pHID->setManufacturer("Teno");
    pHID->setPnp(0x02, 0x045E, 0x0000, 0x0100);  //BT SIG, vendor/product/version genéricos
    pHID->setHidInfo(0x00, 0x01);                 // país 0x00 + normalmente conectado

    //Registrar el descriptor de reporte HID
    pHID->setReportMap(hidReportDescriptor, sizeof(hidReportDescriptor));

    //Input report: canal por donde mandamos las teclas al host (Report ID = 1)
    pInput = pHID->getInputReport(1);

    //Arrancar los servicios HID
    pHID->startServices();

    //Configurar y arrancar advertising
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->setAppearance(HID_KEYBOARD);
    pAdv->addServiceUUID(pHID->getHidService()->getUUID());
    pAdv->start();
}

//Loop
void loop() {
    // Actualiza estado del hardware (teclado, etc.)
    M5Cardputer.update();

    // Estado actual del teclado físico
    auto status = M5Cardputer.Keyboard.keysState();

    // Reporte HID que vamos a armar
    KeyReport report = {0};
    uint8_t index = 0;

    // Recorremos las teclas presionadas
    for (auto raw : status.hid_keys) {

        uint8_t key = raw;

        // Si la tecla viene con SHIFT embebido (bit 7),
        // lo pasamos a modifiers y limpiamos el bit
        if (key & 0x80) {
            report.modifiers |= 0x02;  // LEFT SHIFT
            key &= 0x7F;
        }

        // Capa FN: remapeos especiales
        if (status.fn) {
            switch (key) {

                // Flechas
                case 0x33: key = 0x52; break;  // ; -> UP
                case 0x37: key = 0x51; break;  // . -> DOWN
                case 0x36: key = 0x50; break;  // , -> LEFT
                case 0x38: key = 0x4F; break;  // / -> RIGHT

                // F1 a F12 (fila numérica)
                case 0x1E: key = 0x3A; break;  // 1 -> F1
                case 0x1F: key = 0x3B; break;  // 2 -> F2
                case 0x20: key = 0x3C; break;  // 3 -> F3
                case 0x21: key = 0x3D; break;  // 4 -> F4
                case 0x22: key = 0x3E; break;  // 5 -> F5
                case 0x23: key = 0x3F; break;  // 6 -> F6
                case 0x24: key = 0x40; break;  // 7 -> F7
                case 0x25: key = 0x41; break;  // 8 -> F8
                case 0x26: key = 0x42; break;  // 9 -> F9
                case 0x27: key = 0x43; break;  // 0 -> F10
                case 0x2D: key = 0x44; break;  // - -> F11
                case 0x2E: key = 0x45; break;  // = -> F12

                // Insert
                case 0x2C: key = 0x49; break;  // SPACE -> INSERT

                // Supr (Forward Delete)
                case 0x2A: key = 0x4C; break;  // BACKSPACE -> DEL
            }
        }

        // Convertir ` en ESC
        if (key == 0x35) {
            key = 0x29;
        }

        // Agregar tecla al reporte (máximo 6 teclas simultáneas)
        report.keys[index++] = key;
        if (index >= 6) break;
    }

    // Agregar modifiers reales (ctrl, alt, etc.)
    report.modifiers |= status.modifiers;

    // Enviar solo si el contenido cambió (evita repeticiones)
    // y solo si hay un host conectado por BLE
    if (bleConnected) {
        if (memcmp(&report, &lastReport, sizeof(KeyReport)) != 0) {
            // El reporte BLE lleva: modifiers + reserved + 6 keycodes = 8 bytes
            uint8_t bleReport[8] = {
                report.modifiers,
                report.reserved,
                report.keys[0], report.keys[1], report.keys[2],
                report.keys[3], report.keys[4], report.keys[5]
            };
            pInput->setValue(bleReport, sizeof(bleReport));
            pInput->notify();
            lastReport = report;
        }
    }

    // Actualizar indicadores en pantalla (solo redibuja si algo cambió)
    drawStatus(status.fn, report.modifiers);

    // Pequeño delay para no saturar CPU - capaz se puede implementar una función milis() pero creo que iba a restar en vez de sumar
    delay(10);
}