#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "secrets.h"

// ==========================================
// CONFIGURACIÓN DE PINES Y CONSTANTES
// ==========================================
const int PIN_LED_BUILDIN = 2;          // Pin del LED integrado en la placa ESP32
const int PIN_SENSOR_GAS = 34;          // Pin analógico (GPIO 34) conectado a la salida AO del MQ-2
const int PIN_BUZZER = 25;              // Pin digital (GPIO 25) conectado al Buzzer local
// Nota el buzzer está alimentado a 3.3V 
// ==========================================
// UMBRALES Y CONFIGURACIÓN DE ALERTAS
// ==========================================
// Umbral de gas en valor crudo del ADC (0 - 4095). 
// Ajustado a 1500 asumiendo que tu aire limpio ronda los ~960.
const int UMBRAL_GAS = 1500; 

// Control de tiempo para evitar spam en Telegram (Cooldown en milisegundos)
unsigned long ultimoEnvioTelegram = 0;
const unsigned long COOLDOWN_TELEGRAM = 30000; // 30 segundos de espera mínima entre alertas repetidas

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
void indicarConexionExitosa();
void indicarErrorConexion();
bool enviarMensajeTelegram(String mensaje);
String urlEncode(String str);
void verificarEstabilizacion(int valorActual);
void activarBuzzerAlarma();

void setup() {
  // Inicializar comunicación serial para depuración a 115200 baudios
  Serial.begin(115200);
  delay(1000);

  // Configuración de pines físicos
  pinMode(PIN_LED_BUILDIN, OUTPUT);
  digitalWrite(PIN_LED_BUILDIN, LOW);
  pinMode(PIN_SENSOR_GAS, INPUT);
  
  // Configuración del pin del Buzzer como salida y asegurarlo apagado (HIGH para módulos activos en LOW)
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, HIGH);

  Serial.println("\n==========================================");
  Serial.println("   INICIANDO CONEXIÓN WI-FI + TELEGRAM    ");
  Serial.println("==========================================");
  Serial.printf("Conectando a la red: %s\n", SECRET_WIFI_SSID);

  // Configurar el ESP32 en modo estación (conecta a router Wi-Fi)
  WiFi.mode(WIFI_STA);
  WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASS);

  int intentos = 0;
  // Bucle de espera activa con parpadeo rápido en el LED mientras conecta al Wi-Fi
  while (WiFi.status() != WL_CONNECTED && intentos < MAX_INTENTOS_WIFI) {
    digitalWrite(PIN_LED_BUILDIN, HIGH);
    delay(100);
    digitalWrite(PIN_LED_BUILDIN, LOW);
    delay(100);

    Serial.print(".");
    intentos++;
  }

  Serial.println("");

  // Verificar si la conexión fue exitosa
  if (WiFi.status() == WL_CONNECTED) {
    wifiConectado = true;
    estadoPrevioWiFI = true;
    
    Serial.println("\n[+] Conexión Wi-Fi establecida con éxito.");
    Serial.print("    Dirección IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf("    Calidad señal (RSSI): %d dBm\n", WiFi.RSSI());

    // 3 destellos rápidos de confirmación visual
    indicarConexionExitosa();
    
    // Apagar el LED definitivamente tras la conexión exitosa
    digitalWrite(PIN_LED_BUILDIN, LOW);

    // Notificar al usuario vía Telegram que el sistema arrancó
    enviarMensajeTelegram("🟢 *ESP32 Conectado*\nEl sistema se ha iniciado. Esperando a que el sensor MQ-2 se estabilice...");
    
    // Iniciar cronómetro de estabilización
    tiempoInicioEstabilizacion = millis();
  } else {
    wifiConectado = false;
    estadoPrevioWiFI = false;
    Serial.println("\n[-] Error: No se pudo conectar a la red Wi-Fi.");
    digitalWrite(PIN_LED_BUILDIN, LOW);
  }
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
    indicarConexionExitosa();
    digitalWrite(PIN_LED_BUILDIN, LOW);
    enviarMensajeTelegram("🟢 *ESP32 Reconectado*\nLa conexión Wi-Fi se ha restablecido.");
  }

  // 3. Operación principal si hay red Wi-Fi disponible
  if (wifiConectado) {
    // Lectura analógica en crudo del sensor (0 a 4095)
    int valorCrudo = analogRead(PIN_SENSOR_GAS);
    // Conversión matemática opcional para estimar el voltaje en el pin AO
    float voltaje = valorCrudo * (3.3 / 4095.0);

    Serial.printf("[Sensor MQ-2] Crudo: %d  |  Voltaje: %.2f V", valorCrudo, voltaje);

    // Bifurcación: Si el sensor aún no está listo, evaluamos su calentamiento
    if (!sensorEstabilizado) {
      Serial.print(" [Calentando/Estabilizando...]");
      verificarEstabilizacion(valorCrudo);
      digitalWrite(PIN_BUZZER, HIGH); // Mantener el buzzer apagado (HIGH) durante el arranque
    } 
    // Si ya está listo, pasamos a vigilar activamente la presencia de gas/humo
    else {
      Serial.print(" [Sensor Listo ✅]");
      
      // Evaluar si superamos el umbral de peligro configurado
      if (valorCrudo > UMBRAL_GAS) {
        
        // Activar sirena local de inmediato por hardware
        activarBuzzerAlarma();

        // Verificar si ya transcurrió el tiempo de cooldown para evitar saturar Telegram
        if (millis() - ultimoEnvioTelegram > COOLDOWN_TELEGRAM) {
          Serial.println("\n[!] ¡Nivel de gas elevado detectado! Enviando alerta...");
          bool enviado = enviarMensajeTelegram("🚨 *¡ALERTA DE GAS/HUMO!* \nSe ha detectado una concentración elevada de gas en el ambiente.");
          
          if (enviado) {
            ultimoEnvioTelegram = millis(); // Actualizar marca de tiempo del último envío exitoso
          }
        } else {
          Serial.println(" (En periodo de cooldown, alerta silenciada temporalmente)");
        }
      } else {
        digitalWrite(PIN_BUZZER, HIGH); // Asegurar buzzer apagado (HIGH) si los niveles son normales
      }
    }
    Serial.println("");

    delay(2000); // Pausa de 2 segundos entre cada ciclo de lectura
  } 
  // 4. Si NO hay Wi-Fi, ejecutar rutina de reconexión en segundo plano y parpadeo de error
  else {
    // Si no hay red pero el sensor detecta gas, la sirena local debe sonar de todos modos
    int valorCrudo = analogRead(PIN_SENSOR_GAS);
    if (valorCrudo > UMBRAL_GAS) {
      activarBuzzerAlarma();
    } else {
      digitalWrite(PIN_BUZZER, HIGH);
    }

    WiFi.reconnect();
    indicarErrorConexion();
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
    Serial.printf(" (Variación en 20s: %d)", delta);

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
void activarBuzzerAlarma() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_BUZZER, LOW);  // Encender buzzer (LOW para módulos activos en bajo)
    delay(100);
    digitalWrite(PIN_BUZZER, HIGH); // Apagar buzzer (HIGH)
    delay(100);
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

void indicarErrorConexion() {
  digitalWrite(PIN_LED_BUILDIN, HIGH);
  delay(100);
  digitalWrite(PIN_LED_BUILDIN, LOW);
  delay(100);
  digitalWrite(PIN_LED_BUILDIN, HIGH);
  delay(100);
  digitalWrite(PIN_LED_BUILDIN, LOW);
  delay(800);
}