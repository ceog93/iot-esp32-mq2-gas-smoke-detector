#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h> // Librería para MQTT (Home Assistant)
#include <ArduinoJson.h>  // Librería JSON (asegúrate de tener bblanchon/ArduinoJson en lib_deps)
#include "secrets.h"

// ==========================================
// IDENTIFICACIÓN DEL NODO (ESCALABILIDAD FOG)
// ==========================================
// Se Cambia estos 3 valores para cada nuevo ESP32 que se agregue a la red
#define NODE_ID "cocina"                // Ubicación (usado para armar los tópicos MQTT)
#define DEVICE_ID "esp32_gas_001"       // ID único del hardware para la base de datos de Home Assistant
#define DEVICE_NAME "Sensor Gas Cocina" // Nombre amigable que se mostrará en el panel

// Identificación dentro del namespace de simulación (distinto a esp32-virtual-001..010)
#define SIM_DEVICE_ID "esp32-fisico-001"
#define SIM_UBICACION NODE_ID

// ==========================================
// CONFIGURACIÓN DE PINES Y CONSTANTES
// ==========================================
const int PIN_LED_BUILDIN = 2;          // Pin del LED integrado en la placa ESP32 (Azul)
const int PIN_SENSOR_GAS = 34;          // Pin analógico (GPIO 34) conectado a la salida AO del MQ-2
const int PIN_BUZZER = 25;              // Pin digital (GPIO 25) conectado al Buzzer local
const int PIN_RELE = 26;                // Pin digital (GPIO 26) conectado al Módulo Relé de 5V (3 pines)
const int PIN_VENTILADOR = 14;          // Pin digital (GPIO 14) conectado al Ventilador / driver de ventilación
const bool VENTILADOR_ACTIVO_EN_BAJO = false; // true si activa con LOW, false si activa con HIGH
// Nota el buzzer está alimentado a 3.3V 

// Definición de pines para los LEDs del semáforo
const int PIN_LED_ROJO = 33;            // Pin digital (GPIO 33) conectado al LED Rojo
const int PIN_LED_AMARILLO = 32;        // Pin digital (GPIO 32) conectado al LED Amarillo
const int PIN_LED_VERDE = 27;           // Pin digital (GPIO 27) conectado al LED Verde

// Variables independientes de control de tiempo no bloqueante para cada patrón LED y tareas
unsigned long tiempoUltimoParpadeoVerde = 0;
unsigned long tiempoUltimoParpadeoAmarillo = 0;
unsigned long tiempoUltimoParpadeoSirena = 0;
unsigned long tiempoUltimaLecturaSensor = 0; // Control no bloqueante para el ciclo de 2 segundos
unsigned long tiempoUltimoReintentoWiFi = 0; // Control no bloqueante para la reconexión Wi-Fi

// Variables para el control del LED azul (Wi-Fi)
unsigned long tiempoUltimoReintentoMQTT = 0; // NUEVO: Control no bloqueante para MQTT
unsigned long tiempoUltimoParpadeoAzul = 0;
unsigned long tiempoInicioLedIntento = 0;
bool mostrandoLedIntento = false;
unsigned long tiempoLatidoMQTT = 0;
bool latidoMQTTActivo = false;

bool estadoLedVerdeToggle = false;
bool estadoLedAmarilloToggle = false;
bool estadoSirenaLedToggle = false;

// Variables para estado de simulación, alerta temprana, buzzer preventivo y ventilador
bool alarmaPreventivaActiva = false;
unsigned long ultimaAlertaTempranaRecibidaMs = 0;
const unsigned long VIGENCIA_ALERTA_PREVENTIVA_MS = 3500; // Ventana activa mientras llegan alertas (ej. cada 1s)
unsigned long ultimoPublishSimTelemetriaMs = 0;
const unsigned long INTERVALO_SIM_TELEMETRIA_MS = 2000;  // Mismo ritmo de 2s que los nodos simulados

// Ritmo no bloqueante para el buzzer en "alarma preventiva" (ej. pulso corto de 120ms cada 900ms)
unsigned long tiempoUltimoPulsoPreventivo = 0;
bool buzzerPreventivoEncendido = false;
const unsigned long INTERVALO_BUZZER_PREVENTIVO_MS = 900;
const unsigned long DURACION_PULSO_PREVENTIVO_MS = 120;

// ==========================================
// CONFIGURACIÓN DE MQTT (HOME ASSISTANT)
// ==========================================
const int MQTT_PORT = 1883;

// Concatenación automática de macros para generar los tópicos dinámicos
const char* TOPIC_VALOR_GAS = "casa/" NODE_ID "/gas/valor";
const char* TOPIC_ESTADO_ALARMA = "casa/" NODE_ID "/gas/alarma";
const char* TOPIC_DISPONIBILIDAD = "casa/" NODE_ID "/gas/disponibilidad"; // Tópico para LWT (Last Will)

// Tópicos para integración en simulacion/# (sin tocar casa/cocina/gas/...)
const char* TOPIC_SIM_ESTADO = "simulacion/" SIM_DEVICE_ID "/estado";
const char* TOPIC_SIM_TELEMETRIA = "simulacion/" SIM_DEVICE_ID "/telemetria";
const char* TOPIC_SIM_ALERTA_TEMPRANA = "simulacion/alerta_temprana";

// Tópicos y Payloads para Auto-Discovery de Home Assistant
const char* TOPIC_DISCOVERY_SENSOR = "homeassistant/sensor/" DEVICE_ID "/valor/config";
const char* TOPIC_DISCOVERY_ALARMA = "homeassistant/binary_sensor/" DEVICE_ID "/alarma/config";
const char* TOPIC_DISCOVERY_ESTADO = "homeassistant/binary_sensor/" DEVICE_ID "/estado/config"; // Sensor de Conexión

const char* PAYLOAD_DISCOVERY_SENSOR = 
  "{\"name\":\"Nivel de Gas\","
  "\"unique_id\":\"" DEVICE_ID "_ppm\","
  "\"state_topic\":\"casa/" NODE_ID "/gas/valor\","
  "\"availability_topic\":\"casa/" NODE_ID "/gas/disponibilidad\","
  "\"payload_available\":\"online\","
  "\"payload_not_available\":\"offline\","
  "\"unit_of_measurement\":\"ADC\","
  "\"state_class\":\"measurement\","
  "\"icon\":\"mdi:gas-cylinder\","
  "\"device\":{\"identifiers\":[\"" DEVICE_ID "\"],\"name\":\"" DEVICE_NAME "\",\"model\":\"MQ-2 ESP32\",\"manufacturer\":\"Homelab\"}}";

const char* PAYLOAD_DISCOVERY_ALARMA = 
  "{\"name\":\"Alarma de Gas\","
  "\"unique_id\":\"" DEVICE_ID "_alerta\","
  "\"state_topic\":\"casa/" NODE_ID "/gas/alarma\","
  "\"availability_topic\":\"casa/" NODE_ID "/gas/disponibilidad\","
  "\"payload_available\":\"online\","
  "\"payload_not_available\":\"offline\","
  "\"payload_on\":\"ON\","
  "\"payload_off\":\"OFF\","
  "\"device_class\":\"gas\","
  "\"device\":{\"identifiers\":[\"" DEVICE_ID "\"]}}";

const char* PAYLOAD_DISCOVERY_ESTADO = 
  "{\"name\":\"Conexión ESP32\","
  "\"unique_id\":\"" DEVICE_ID "_conexion\","
  "\"state_topic\":\"casa/" NODE_ID "/gas/disponibilidad\","
  "\"payload_on\":\"online\","
  "\"payload_off\":\"offline\","
  "\"device_class\":\"connectivity\","
  "\"entity_category\":\"diagnostic\","
  "\"device\":{\"identifiers\":[\"" DEVICE_ID "\"]}}";

WiFiClient espClient; // Cliente Wi-Fi normal (sin encriptar) para conexión local MQTT
PubSubClient mqttClient(espClient);

// ==========================================
// UMBRALES Y CONFIGURACIÓN DE ALERTAS
// ==========================================
// Umbral de gas en valor crudo del ADC (0 - 4095). 
// Ajustado a 1500 asumiendo que tu aire limpio ronda los ~960.
const int UMBRAL_GAS = 1500; 

// Control de tiempo para evitar spam en Telegram (Cooldown en milisegundos)
unsigned long ultimoEnvioTelegram = 0;
const unsigned long COOLDOWN_TELEGRAM = 30000; // 30 segundos de espera mínima entre alertas repetidas
const unsigned long INTERVALO_REINTENTO_WIFI = 10000; // 10 segundos entre cada intento de reconexión
const unsigned long INTERVALO_REINTENTO_MQTT = 5000;  // 5 segundos entre reintentos MQTT

// ==========================================
// VARIABLES PARA ESTABILIZACIÓN AUTOMÁTICA
// ==========================================
bool sensorEstabilizado = false;         // Bandera para saber si el sensor ya superó el calentamiento
int ultimasLecturas[10];                // Arreglo para guardar una ventana deslizante de las últimas 10 lecturas
int indiceLectura = 0;                  // Índice actual dentro del arreglo circular
bool arregloLleno = false;              // Indica si ya recolectamos suficientes datos para empezar a evaluar
unsigned long tiempoInicioEstabilizacion = 0; // Cronómetro para medir cuánto tardó en estabilizarse

// ==========================================
// PARÁMETROS DE CONEXIÓN WI-FI
// ==========================================
const int MAX_INTENTOS_WIFI = 25;       // Número máximo de intentos antes de dar error de conexión
bool wifiConectado = false;             // Estado actual de la red
bool estadoPrevioWiFI = false;          // Memoria del estado anterior para detectar caídas/recuperaciones

// ==========================================
// PROTOTIPOS DE FUNCIONES
// ==========================================
void inicializarPines();
void iniciarConexionWiFi();
void indicarConexionExitosa();
bool enviarMensajeTelegram(String mensaje);
String urlEncode(String str);
void verificarEstabilizacion(int valorActual);
void activarBuzzerAlarma();
void conectarMQTT(); 
void enviarConfiguracionDiscovery();
void gestionarSistemaGas(int valorCrudo, bool hayRedWiFi); 
void callbackMQTT(char* topic, byte* payload, unsigned int length);
void actualizarAlarmaPreventiva();
void publicarTelemetriaSimulacion(int valorCrudo, const char* estadoLectura);
void configurarVentilador(bool encendido);
void actualizarBuzzerPreventivo();
void apagarBuzzerPreventivo();

// Funciones auxiliares para evitar redundancia
void configurarLEDs(bool rojo, bool amarillo, bool verde);
void asegurarEstadoSeguro();
void reportarEstadoMQTT(bool hayRedWiFi, const char* estado);
void gestionarAlertaTelegram(bool hayRedWiFi);
void imprimirLecturaSensor(int valorCrudo, const char* sufijo);

void setup() {
  // Inicializar comunicación serial para depuración a 115200 baudios
  Serial.begin(115200);
  delay(1000);

  // Modularización: Inicializar todos los pines de hardware
  inicializarPines();

  // Configuración inicial de la librería MQTT antes de cualquier tráfico de red
  mqttClient.setServer(SECRET_MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(callbackMQTT);
  mqttClient.setBufferSize(1200); // Asignación limpia de memoria estática (Aumentado ligeramente para el 3er sensor)

  Serial.println("\n==========================================");
  Serial.println("   INICIANDO CONEXIÓN WI-FI + TELEGRAM    ");
  Serial.println("==========================================");
  
  // Modularización: Proceso de conexión a la red
  iniciarConexionWiFi();
}

void loop() {
  bool estadoActualWiFi = (WiFi.status() == WL_CONNECTED);

  // 1. Detectar si hubo una pérdida repentina de conexión Wi-Fi
  if (estadoPrevioWiFI && !estadoActualWiFi) {
    Serial.println("[!] ¡Alerta! Conexión Wi-Fi perdida.");
    wifiConectado = false;
    estadoPrevioWiFI = false;
  } 
  // 2. Detectar si se recuperó la conexión Wi-Fi previamente caída
  else if (!estadoPrevioWiFI && estadoActualWiFi) {
    Serial.println("[+] ¡Conexión Wi-Fi recuperada!");
    wifiConectado = true;
    estadoPrevioWiFI = true;
    mostrandoLedIntento = false; // Cancelar estado de intento LED
    indicarConexionExitosa();    // Confirmación visual
    digitalWrite(PIN_LED_BUILDIN, LOW); // Apaga el LED azul
    enviarMensajeTelegram("🟢 *ESP32 Reconectado*\nLa conexión Wi-Fi se ha restablecido.");
  }

  // Lectura analógica en crudo del sensor (0 a 4095)
  int valorCrudo = analogRead(PIN_SENSOR_GAS);

  // Evaluar si la ventana de "alarma preventiva" sigue vigente mientras llegan alertas tempranas
  actualizarAlarmaPreventiva();

  // 3. Operación principal si hay red Wi-Fi disponible
  if (wifiConectado) {
    
    // GESTIÓN VISUAL MQTT (LED AZUL)
    if (!mqttClient.connected()) {
      
      // Error de MQTT: Doble destello cada 4 segundos
      unsigned long t = millis() % 4000;
      if (t < 150) digitalWrite(PIN_LED_BUILDIN, HIGH);
      else if (t < 300) digitalWrite(PIN_LED_BUILDIN, LOW);
      else if (t < 450) digitalWrite(PIN_LED_BUILDIN, HIGH);
      else digitalWrite(PIN_LED_BUILDIN, LOW);

      // Reintento no bloqueante: Solo intenta conectar cada 5 segundos
      if (millis() - tiempoUltimoReintentoMQTT >= INTERVALO_REINTENTO_MQTT) {
        tiempoUltimoReintentoMQTT = millis();
        
        digitalWrite(PIN_LED_BUILDIN, LOW); // <--- SOLUCIÓN: Forzar apagado antes de que el procesador se congele buscando el servidor
        conectarMQTT();
      }

    } else {
      // Todo OK: Apagar el LED azul si el latido de 50ms ya terminó
      if (latidoMQTTActivo && (millis() - tiempoLatidoMQTT >= 50)) {
        digitalWrite(PIN_LED_BUILDIN, LOW);
        latidoMQTTActivo = false;
      }
      mqttClient.loop(); // Permite a la librería MQTT procesar datos entrantes/salientes
    }

    // Enviar el valor numérico en tiempo real a Home Assistant (4 veces por segundo) para evitar flooding
    static unsigned long tiempoUltimoPublishValor = 0;
    if (millis() - tiempoUltimoPublishValor >= 250) {
      tiempoUltimoPublishValor = millis();
      if (mqttClient.connected()) {
        mqttClient.publish(TOPIC_VALOR_GAS, String(valorCrudo).c_str());
      }
    }

    // Ejecutar lógica central unificada indicando que SÍ hay red
    gestionarSistemaGas(valorCrudo, true);
  } 
  // 4. Si NO hay Wi-Fi, ejecutar rutina de reconexión periódica y lógica central de gas
  else {
    gestionarSistemaGas(valorCrudo, false);

    // ==============================================================
    // CONTROL DEL LED AZUL SIN CONEXIÓN (Modo Offline / Reconectando)
    // ==============================================================
    if (mostrandoLedIntento) {
      // Mantener el LED azul constante por 2.5 segundos tras el intento
      digitalWrite(PIN_LED_BUILDIN, HIGH);
      if (millis() - tiempoInicioLedIntento >= 2500) {
        mostrandoLedIntento = false; // Termina el tiempo sólido, vuelve a parpadear
      }
    } else {
      // Parpadeo rápido (100 ms) igual al inicio cuando no hay red
      if (millis() - tiempoUltimoParpadeoAzul >= 100) {
        tiempoUltimoParpadeoAzul = millis();
        digitalWrite(PIN_LED_BUILDIN, !digitalRead(PIN_LED_BUILDIN));
      }
    }

    // ==============================================================
    // SISTEMA DE RECONEXIÓN NO BLOQUEANTE (Cada 10 segundos)
    // ==============================================================
    if (millis() - tiempoUltimoReintentoWiFi >= INTERVALO_REINTENTO_WIFI) {
      tiempoUltimoReintentoWiFi = millis();
      
      // Activar bandera para poner el LED azul constante por 2.5 seg
      tiempoInicioLedIntento = millis();
      mostrandoLedIntento = true; 
      
      Serial.println("[Wi-Fi] Intentando reconectar a la red...");
      WiFi.disconnect();
      WiFi.reconnect();
    }
  }
}

// ==========================================
// FUNCIONES DE INTEGRACIÓN CON SIMULACION/#
// ==========================================
void actualizarAlarmaPreventiva() {
  if (alarmaPreventivaActiva && (millis() - ultimaAlertaTempranaRecibidaMs > VIGENCIA_ALERTA_PREVENTIVA_MS)) {
    alarmaPreventivaActiva = false;
    apagarBuzzerPreventivo();
    Serial.println("[Simulación] Ventana de alerta temprana expirada -> volviendo a estado normal.");
  }
}

void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_SIM_ALERTA_TEMPRANA) != 0) {
    return;
  }

  // Copiar payload a String segura
  String jsonStr;
  jsonStr.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    jsonStr += (char)payload[i];
  }

  // Extraer origen_device_id de forma ligera sin depender de librerías extra
  int idxOrigen = jsonStr.indexOf("\"origen_device_id\"");
  String origenId = "";
  if (idxOrigen != -1) {
    int colonIdx = jsonStr.indexOf(":", idxOrigen);
    int firstQuote = jsonStr.indexOf("\"", colonIdx);
    int secondQuote = jsonStr.indexOf("\"", firstQuote + 1);
    if (firstQuote != -1 && secondQuote != -1) {
      origenId = jsonStr.substring(firstQuote + 1, secondQuote);
    }
  }

  // Ignorar alertas emitidas por este mismo nodo para evitar bucles
  if (origenId == SIM_DEVICE_ID || origenId == DEVICE_ID) {
    return;
  }

  // Mantener estado de lectura en "alarma preventiva" mientras se reciban alertas tempranas
  // sin modificar el ADC real ni republicar en simulacion/alerta_temprana
  alarmaPreventivaActiva = true;
  ultimaAlertaTempranaRecibidaMs = millis();
}

void publicarTelemetriaSimulacion(int valorCrudo, const char* estadoLectura) {
  if (!mqttClient.connected()) return;
  if (millis() - ultimoPublishSimTelemetriaMs < INTERVALO_SIM_TELEMETRIA_MS) return;

  ultimoPublishSimTelemetriaMs = millis();

  // Timestamp relativo/ISO aproximado si no hay NTP sincronizado, o millis si prefieres
  char payloadSim[256];
  snprintf(
    payloadSim,
    sizeof(payloadSim),
    "{\"device_id\":\"%s\",\"ubicacion\":\"%s\",\"adc\":%d,\"estado\":\"%s\",\"uptime_ms\":%lu,\"simulado\":false}",
    SIM_DEVICE_ID,
    SIM_UBICACION,
    valorCrudo,
    estadoLectura,
    millis()
  );

  mqttClient.publish(TOPIC_SIM_TELEMETRIA, payloadSim);
}

void configurarVentilador(bool encendido) {
  uint8_t nivel = encendido
    ? (VENTILADOR_ACTIVO_EN_BAJO ? LOW : HIGH)
    : (VENTILADOR_ACTIVO_EN_BAJO ? HIGH : LOW);
  digitalWrite(PIN_VENTILADOR, nivel);
}

void apagarBuzzerPreventivo() {
  buzzerPreventivoEncendido = false;
  digitalWrite(PIN_BUZZER, HIGH); // Buzzer apagado (HIGH)
}

void actualizarBuzzerPreventivo() {
  unsigned long ahora = millis();
  if (!buzzerPreventivoEncendido) {
    if (ahora - tiempoUltimoPulsoPreventivo >= INTERVALO_BUZZER_PREVENTIVO_MS) {
      tiempoUltimoPulsoPreventivo = ahora;
      buzzerPreventivoEncendido = true;
      digitalWrite(PIN_BUZZER, LOW); // Pulso preventivo corto (activo en bajo)
    }
  } else {
    if (ahora - tiempoUltimoPulsoPreventivo >= DURACION_PULSO_PREVENTIVO_MS) {
      buzzerPreventivoEncendido = false;
      digitalWrite(PIN_BUZZER, HIGH); // Apagar buzzer tras el pulso preventivo
    }
  }
}

// ==========================================
// FUNCIONES AUXILIARES PARA ELIMINAR REDUNDANCIA
// ==========================================
void inicializarPines() {
  pinMode(PIN_LED_BUILDIN, OUTPUT);
  digitalWrite(PIN_LED_BUILDIN, LOW);
  pinMode(PIN_SENSOR_GAS, INPUT);
  
  pinMode(PIN_LED_ROJO, OUTPUT);
  pinMode(PIN_LED_AMARILLO, OUTPUT);
  pinMode(PIN_LED_VERDE, OUTPUT);
  configurarLEDs(false, false, false); // Inicializar los LEDs apagados

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, HIGH); // Apagado por defecto
  pinMode(PIN_RELE, INPUT);       // Alta impedancia para cortar fuga

  pinMode(PIN_VENTILADOR, OUTPUT);
  configurarVentilador(false);    // Ventilador apagado por defecto
}

void iniciarConexionWiFi() {
  Serial.printf("Conectando a la red: %s\n", SECRET_WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASS);

  int intentos = 0;
  // Bucle de espera activa con parpadeo rápido en el LED mientras conecta al Wi-Fi (solo en Setup)
  while (WiFi.status() != WL_CONNECTED && intentos < MAX_INTENTOS_WIFI) {
    digitalWrite(PIN_LED_BUILDIN, HIGH);
    delay(100);
    digitalWrite(PIN_LED_BUILDIN, LOW);
    delay(100);
    Serial.print(".");
    intentos++;
  }
  Serial.println("");

  if (WiFi.status() == WL_CONNECTED) {
    wifiConectado = true;
    estadoPrevioWiFI = true;
    
    Serial.println("\n[+] Conexión Wi-Fi establecida con éxito.");
    Serial.print("    Dirección IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf("    Calidad señal (RSSI): %d dBm\n", WiFi.RSSI());

    indicarConexionExitosa();
    digitalWrite(PIN_LED_BUILDIN, LOW);

    enviarMensajeTelegram("🟢 *ESP32 Conectado*\nEl sistema se ha iniciado. Esperando a que el sensor MQ-2 se estabilice...");
    tiempoInicioEstabilizacion = millis();
  } else {
    wifiConectado = false;
    estadoPrevioWiFI = false;
    Serial.println("\n[-] Error: No se pudo conectar a la red Wi-Fi. Iniciando en modo offline.");
    digitalWrite(PIN_LED_BUILDIN, LOW);
  }
}

void configurarLEDs(bool rojo, bool amarillo, bool verde) {
  digitalWrite(PIN_LED_ROJO, rojo ? HIGH : LOW);
  digitalWrite(PIN_LED_AMARILLO, amarillo ? HIGH : LOW);
  digitalWrite(PIN_LED_VERDE, verde ? HIGH : LOW);
}

void asegurarEstadoSeguro() {
  digitalWrite(PIN_BUZZER, HIGH); // Mantener el buzzer apagado (HIGH) 
  pinMode(PIN_RELE, INPUT);       // Mantener relé en alta impedancia (apagado)
}

void reportarEstadoMQTT(bool hayRedWiFi, const char* estado) {
  // Lógica Anti-Saturación: Publica inmediatamente al cambiar el estado, 
  // o refuerza el aviso cada 2 segundos como latido de seguridad.
  static String ultimoEstadoReportado = "";
  static unsigned long tiempoUltimoReporteEstado = 0;

  if (ultimoEstadoReportado != estado || (millis() - tiempoUltimoReporteEstado >= 2000)) {
    if (hayRedWiFi && mqttClient.connected()) {
      mqttClient.publish(TOPIC_ESTADO_ALARMA, estado);
      ultimoEstadoReportado = estado;
      tiempoUltimoReporteEstado = millis();
    }
  }
}

void gestionarAlertaTelegram(bool hayRedWiFi) {
  if (hayRedWiFi) {
    if (millis() - ultimoEnvioTelegram > COOLDOWN_TELEGRAM) {
      Serial.println("[!] ¡Nivel de gas elevado detectado! Enviando alerta...");
      bool enviado = enviarMensajeTelegram("🚨 *¡ALERTA DE GAS/HUMO!* \nSe ha detectado una concentración elevada de gas en el ambiente.");
      if (enviado) {
        ultimoEnvioTelegram = millis(); // Actualizar marca de tiempo del último envío exitoso
      }
    } else {
      Serial.println(" (En periodo de cooldown, alerta silenciada temporalmente)");
    }
  }
}

void imprimirLecturaSensor(int valorCrudo, const char* sufijo) {
  float voltaje = valorCrudo * (3.3 / 4095.0);
  Serial.printf("[Sensor MQ-2] Crudo: %d  |  Voltaje: %.2f V%s\n", valorCrudo, voltaje, sufijo);
}

// ==========================================
// FUNCIÓN UNIFICADA PARA GESTIÓN DE GAS Y ACTUADORES
// ==========================================
void gestionarSistemaGas(int valorCrudo, bool hayRedWiFi) {
  // Bandera para ejecutar tareas lentas/reportes solo cada 2 segundos sin bloquear el ciclo de LEDs
  bool esMomentoDeReportar = false;

  if (millis() - tiempoUltimaLecturaSensor >= 2000) {
    tiempoUltimaLecturaSensor = millis();
    esMomentoDeReportar = true;
    
    // Disparar Latido Azul si hay red y Home Assistant está conectado
    if (hayRedWiFi && mqttClient.connected()) {
      digitalWrite(PIN_LED_BUILDIN, HIGH);
      tiempoLatidoMQTT = millis();
      latidoMQTTActivo = true;
    }
  }

  // Bifurcación: Si el sensor aún no está listo, evaluamos su calentamiento
  if (!sensorEstabilizado) {
    if (esMomentoDeReportar) {
      imprimirLecturaSensor(valorCrudo, " [Calentando/Estabilizando...]");
      verificarEstabilizacion(valorCrudo);
    }
    
    asegurarEstadoSeguro();
    configurarVentilador(false);

    // Reportar estado seguro a HA en paralelo al calentamiento
    reportarEstadoMQTT(hayRedWiFi, "OFF"); 
    if (hayRedWiFi) {
      publicarTelemetriaSimulacion(valorCrudo, alarmaPreventivaActiva ? "alarma preventiva" : "normal");
    }
    
    // Durante el calentamiento: LED amarillo intermitente rápido a 125 ms (fluido sin bloqueos)
    if (millis() - tiempoUltimoParpadeoAmarillo >= 125) {
      tiempoUltimoParpadeoAmarillo = millis();
      estadoLedAmarilloToggle = !estadoLedAmarilloToggle;
    }
    configurarLEDs(false, estadoLedAmarilloToggle, false);
  } 
  // Si ya está listo, pasamos a vigilar activamente la presencia de gas/humo
  else {
    // Evaluar si superamos el umbral de peligro configurado
    if (valorCrudo > UMBRAL_GAS) {
      apagarBuzzerPreventivo();
      configurarVentilador(true); // Encender ventilador en alarma propia

      // Control visual de LEDs: Alerta detectada -> Amarillo y Rojo intermitentes intercalados muy rápido a 60 ms (sirena ultrarrápida)
      if (millis() - tiempoUltimoParpadeoSirena >= 60) { 
        tiempoUltimoParpadeoSirena = millis();
        estadoSirenaLedToggle = !estadoSirenaLedToggle;
      }
      configurarLEDs(estadoSirenaLedToggle, !estadoSirenaLedToggle, false);
      
      reportarEstadoMQTT(hayRedWiFi, "ON");
      if (hayRedWiFi) {
        // El nodo que supera el umbral mantiene su estado de lectura en "alarma"
        publicarTelemetriaSimulacion(valorCrudo, "alarma");
      }

      // Activar relé y sirena local INMEDIATAMENTE
      pinMode(PIN_RELE, OUTPUT);
      digitalWrite(PIN_RELE, LOW);
      activarBuzzerAlarma();

      if (esMomentoDeReportar) {
        imprimirLecturaSensor(valorCrudo, " [Sensor Listo ✅]");
        gestionarAlertaTelegram(hayRedWiFi);
      }
    } else {
      // Sin detección propia: puede estar en "alarma preventiva" (si llegan alertas tempranas de otro nodo) o "normal"
      const char* estadoSim = alarmaPreventivaActiva ? "alarma preventiva" : "normal";

      if (alarmaPreventivaActiva) {
        // Indicador preventivo: LED amarillo intermitente + pulso de buzzer suave + ventilador activo, sin activar relé de corte
        if (millis() - tiempoUltimoParpadeoAmarillo >= 200) {
          tiempoUltimoParpadeoAmarillo = millis();
          estadoLedAmarilloToggle = !estadoLedAmarilloToggle;
        }
        configurarLEDs(false, estadoLedAmarilloToggle, false);
        actualizarBuzzerPreventivo();
        configurarVentilador(true);
        digitalWrite(PIN_RELE, HIGH); // Asegura relé en reposo si se había configurado como salida
        pinMode(PIN_RELE, INPUT);
      } else {
        // Control visual de LEDs: Sin detección de gas y estabilizado -> LED verde intermitente como un beacon (1000ms)
        if (millis() - tiempoUltimoParpadeoVerde >= 1000) { 
          tiempoUltimoParpadeoVerde = millis();
          estadoLedVerdeToggle = !estadoLedVerdeToggle;
        }
        configurarLEDs(false, false, estadoLedVerdeToggle);
        apagarBuzzerPreventivo();
        configurarVentilador(false);
        asegurarEstadoSeguro();
      }
      
      reportarEstadoMQTT(hayRedWiFi, "OFF");
      if (hayRedWiFi) {
        publicarTelemetriaSimulacion(valorCrudo, estadoSim);
      }

      if (esMomentoDeReportar) {
        imprimirLecturaSensor(valorCrudo, alarmaPreventivaActiva ? " [Sensor Listo ✅ | Alarma Preventiva ⚠️]" : " [Sensor Listo ✅]");
      }
    }
  }
}

// ==========================================
// FUNCIÓN PARA ENVIAR AUTO-DISCOVERY A HOME ASSISTANT
// ==========================================
void enviarConfiguracionDiscovery() {
  Serial.print(" [Registrando entidades en Home Assistant...]");
  // El tercer parámetro 'true' retiene el mensaje en Mosquitto para que HA lo recuerde tras reinicios
  mqttClient.publish(TOPIC_DISCOVERY_SENSOR, PAYLOAD_DISCOVERY_SENSOR, true);
  mqttClient.publish(TOPIC_DISCOVERY_ALARMA, PAYLOAD_DISCOVERY_ALARMA, true);
  mqttClient.publish(TOPIC_DISCOVERY_ESTADO, PAYLOAD_DISCOVERY_ESTADO, true);
}

// ==========================================
// FUNCIÓN PARA CONECTAR Y RECONECTAR A MQTT
// ==========================================
void conectarMQTT() {
  Serial.print(" [Conectando a MQTT...]");
  // Crear un ID de cliente aleatorio pero identificable en Mosquitto
  String clientId = "ESP32-" + String(NODE_ID) + "-";
  clientId += String(random(0xffff), HEX);
  
  // Intentar conectar configurando LWT (tópico, QoS 0, retenido true, mensaje "offline")
  if (mqttClient.connect(clientId.c_str(), SECRET_MQTT_USER, SECRET_MQTT_PASS, TOPIC_DISPONIBILIDAD, 0, true, "offline")) {
    Serial.print(" [MQTT ✅]\n");
    
    // Publicar inmediatamente que el dispositivo está online (retenido true)
    mqttClient.publish(TOPIC_DISPONIBILIDAD, "online", true);

    // Publicar disponibilidad retenida en simulacion/<SIM_DEVICE_ID>/estado ("conectado")
    mqttClient.publish(TOPIC_SIM_ESTADO, "conectado", true);

    // Suscribirse al topic de alerta temprana para reaccionar sin republicar en él
    mqttClient.subscribe(TOPIC_SIM_ALERTA_TEMPRANA);
    
    enviarConfiguracionDiscovery(); // Enviar registro automático al broker
  } else {
    Serial.print(" [MQTT ❌ Error ");
    Serial.print(mqttClient.state());
    Serial.print("]\n");
  }
}

// ==========================================
// FUNCIÓN DE ESTABILIZACIÓN AUTOMÁTICA
// ==========================================
void verificarEstabilizacion(int valorActual) {
  // Almacenar lectura actual en el arreglo circular
  ultimasLecturas[indiceLectura] = valorActual;
  indiceLectura = (indiceLectura + 1) % 10;

  if (indiceLectura == 0) {
    arregloLleno = true; // Marcamos que ya tenemos una ronda completa de 10 muestras (20 segundos)
  }

  // Analizar únicamente cuando el buffer circular esté completamente lleno
  if (arregloLleno) {
    int minVal = ultimasLecturas[0];
    int maxVal = ultimasLecturas[0];

    // Buscar el valor mínimo y máximo dentro de las últimas 10 mediciones
    for (int i = 1; i < 10; i++) {
      if (ultimasLecturas[i] < minVal) minVal = ultimasLecturas[i];
      if (ultimasLecturas[i] > maxVal) maxVal = ultimasLecturas[i];
    }

    int delta = maxVal - minVal; // Calcular la fluctuación máxima (delta)
    Serial.printf(" (Variación en 20s: %d)\n", delta);

    // =========================================================================
    // SELECCIÓN DE TOLERANCIA DE ESTABILIZACIÓN:
    // =========================================================================
    // OPCIÓN A (PRODUCCIÓN / ALTA PRECISIÓN): 
    // delta <= 8 -> Exige una curva muy plana (tarda más en arrancar, ~15-20 min).
    // -------------------------------------------------------------------------
    // OPCIÓN B (PRUEBAS / MODO RÁPIDO): 
    // delta <= 15 -> Tolera mayor fluctuación (arranca en pocos minutos).
    // =========================================================================
    
    int toleranciaEstabilidad = 15; // Cambiar a 8 cuando pase a PRODUCCIÓN
    
    if (delta <= toleranciaEstabilidad) {
      sensorEstabilizado = true;
      unsigned long tiempoTotalSegundos = (millis() - tiempoInicioEstabilizacion) / 1000;
      Serial.println("\n[+] ¡Sensor MQ-2 estabilizado con éxito (modo rápido)!");
      
      // Construir y enviar mensaje de confirmación a Telegram
      String msg = "✅ *Sensor MQ-2 Estabilizado*\nCalibración rápida completada.\nValor base de aire limpio: " + String(valorActual) + "\nTiempo transcurrido: " + String(tiempoTotalSegundos) + " segundos.";
      enviarMensajeTelegram(msg);
    }
  }
}

// ==========================================
// FUNCIÓN AUXILIAR: URL ENCODE
// ==========================================
// Permite formatear caracteres especiales (espacios, tildes, símbolos) para enviarlos por URL HTTP GET
String urlEncode(String str) {
  String encodedString = "";
  char c;
  char code0;
  char code1;
  for (unsigned int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c)) {
      encodedString += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) code0 = c - 10 + 'A';
      encodedString += '%';
      encodedString += code0;
      encodedString += code1;
    }
  }
  return encodedString;
}

// ==========================================
// FUNCIÓN DE ENVÍO A TELEGRAM VÍA HTTPS (Con Reintentos)
// ==========================================
bool enviarMensajeTelegram(String mensaje) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = "/bot" + String(SECRET_BOT_TOKEN) + "/sendMessage?chat_id=" + String(SECRET_CHAT_ID) + "&text=" + urlEncode(mensaje) + "&parse_mode=Markdown";

  // Intentar hasta 3 veces en caso de fallo de red intermitente
  for (int intento = 1; intento <= 3; intento++) {
    Serial.printf("[Telegram] Intentando enviar mensaje (Intento %d/3)...\n", intento);
    
    WiFiClientSecure client;
    client.setInsecure(); // Omitir validación estricta de certificados SSL

    if (client.connect("api.telegram.org", 443)) {
      client.print(String("GET ") + url + " HTTP/1.1\r\n" +
                   "Host: api.telegram.org\r\n" +
                   "Connection: close\r\n\r\n");
      
      unsigned long timeout = millis();
      while (client.connected() && millis() - timeout < 5000) {
        if (client.available()) {
          String line = client.readStringUntil('\n');
          if (line.startsWith("HTTP/1.1 200 OK")) {
            Serial.println("[Telegram] ¡Mensaje enviado con éxito!");
            client.stop();
            return true; // Éxito, salimos de la función
          }
        }
      }
      client.stop();
    }
    
    // Pequeña pausa antes de reintentar
    delay(1000);
  }
  
  Serial.println("[Telegram] Error persistente al enviar el mensaje tras 3 intentos.");
  return false;
}

// ==========================================
// FUNCIÓN PARA ACTIVAR EL BUZZER LOCAL
// ==========================================
void activarBuzzerAlarma_1() {
  //Patrón de tres tonos cortos y agudos (alerta rápida)
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_BUZZER, LOW);  // Encender buzzer (LOW para módulos activos en bajo)
    delay(100);
    digitalWrite(PIN_BUZZER, HIGH); // Apagar buzzer (HIGH)
    delay(100);
  }
}

// ==========================================
// FUNCIÓN PARA ACTIVAR EL BUZZER LOCAL (Sirena de Emergencia)
// ==========================================
void activarBuzzerAlarma_2() {
  // Patrón de alerta tipo despertador (5 pulsos rápidos y estridentes)
  for (int i = 0; i < 5; i++) {
    digitalWrite(PIN_BUZZER, LOW);  // Encender buzzer
    delay(80);                      // Pulso corto
    digitalWrite(PIN_BUZZER, HIGH); // Apagar buzzer
    delay(80);
  }
  delay(200); // Breve pausa entre ráfagas de sirena
}

// ==========================================
// FUNCIÓN PARA ACTIVAR EL BUZZER LOCAL (Patrón Sirena de Patrulla)
// ==========================================
void activarBuzzerAlarma() {
  // Ráfaga rápida 1 (como alerta inicial)
  for (int i = 0; i < 4; i++) {
    digitalWrite(PIN_BUZZER, LOW);  // Encender buzzer
    delay(60);                      // Pulso muy rápido
    digitalWrite(PIN_BUZZER, HIGH); // Apagar buzzer
    delay(60);
  }
  
  // Ráfaga lenta 2 (simulando el cambio de tono de sirena)
  for (int i = 0; i < 2; i++) {
    digitalWrite(PIN_BUZZER, LOW);  // Encender buzzer
    delay(200);                     // Pulso largo
    digitalWrite(PIN_BUZZER, HIGH); // Apagar buzzer
    delay(150);
  }
}

// ==========================================
// FUNCIONES DE INDICADORES VISUALES (LED)
// ==========================================
void indicarConexionExitosa() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_LED_BUILDIN, HIGH);
    delay(150);
    digitalWrite(PIN_LED_BUILDIN, LOW);
    delay(150);
  }
}