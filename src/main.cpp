#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "secrets.h"

// Pin del LED integrado
const int PIN_LED_BUILDIN = 2;

// Pin del sensor MQ-2 (Salida Analógica AO)
const int PIN_SENSOR_GAS = 34;

// Umbral de gas/humo para alerta
const int UMBRAL_GAS = 2000; 

// Control de tiempo para evitar spam en Telegram (Cooldown)
unsigned long ultimoEnvioTelegram = 0;
const unsigned long COOLDOWN_TELEGRAM = 30000; // 30 segundos

// Variables para la detección de estabilización automática
bool sensorEstabilizado = false;
int ultimasLecturas[10]; // Guardamos las últimas 10 lecturas
int indiceLectura = 0;
bool arregloLleno = false;
unsigned long tiempoInicioEstabilizacion = 0;

// Parámetros de conexión
const int MAX_INTENTOS_WIFI = 25;
bool wifiConectado = false;
bool estadoPrevioWiFI = false;

// Prototipos de funciones
void indicarConexionExitosa();
void indicarErrorConexion();
bool enviarMensajeTelegram(String mensaje);
String urlEncode(String str);
void verificarEstabilizacion(int valorActual);

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PIN_LED_BUILDIN, OUTPUT);
  digitalWrite(PIN_LED_BUILDIN, LOW);
  pinMode(PIN_SENSOR_GAS, INPUT);

  Serial.println("\n==========================================");
  Serial.println("   INICIANDO CONEXIÓN WI-FI + TELEGRAM    ");
  Serial.println("==========================================");
  Serial.printf("Conectando a la red: %s\n", SECRET_WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(SECRET_WIFI_SSID, SECRET_WIFI_PASS);

  int intentos = 0;
  // Parpadeo rápido mientras intenta conectar
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

    // 3 destellos rápidos de confirmación
    indicarConexionExitosa();
    
    // Apagar el LED definitivamente
    digitalWrite(PIN_LED_BUILDIN, LOW);

    enviarMensajeTelegram("🟢 *ESP32 Conectado*\nEl sistema se ha iniciado. Esperando a que el sensor MQ-2 se estabilice...");
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

  // Detectar si hubo un cambio de estado (transición de conectado a desconectado)
  if (estadoPrevioWiFI && !estadoActualWiFi) {
    Serial.println("[!] ¡Alerta! Conexión Wi-Fi perdida.");
    wifiConectado = false;
    estadoPrevioWiFI = false;
  } 
  // Detectar si se recuperó la conexión
  else if (!estadoPrevioWiFI && estadoActualWiFi) {
    Serial.println("[+] ¡Conexión Wi-Fi recuperada!");
    wifiConectado = true;
    estadoPrevioWiFI = true;
    indicarConexionExitosa();
    digitalWrite(PIN_LED_BUILDIN, LOW);
    enviarMensajeTelegram("🟢 *ESP32 Reconectado*\nLa conexión Wi-Fi se ha restablecido.");
  }

  // Si hay Wi-Fi, ejecutamos la lectura del sensor y lógica de alertas
  if (wifiConectado) {
    int valorCrudo = analogRead(PIN_SENSOR_GAS);
    float voltaje = valorCrudo * (3.3 / 4095.0);

    Serial.printf("[Sensor MQ-2] Crudo: %d  |  Voltaje: %.2f V", valorCrudo, voltaje);

    // Si aún no se declara estabilizado, evaluamos su comportamiento
    if (!sensorEstabilizado) {
      Serial.print(" [Calentando/Estabilizando...]");
      verificarEstabilizacion(valorCrudo);
    } else {
      Serial.print(" [Sensor Listo ✅]");
      
      // Evaluar presencia de gas solo si ya está listo
      if (valorCrudo > UMBRAL_GAS) {
        if (millis() - ultimoEnvioTelegram > COOLDOWN_TELEGRAM) {
          Serial.println("\n[!] ¡Nivel de gas elevado detectado! Enviando alerta...");
          bool enviado = enviarMensajeTelegram("🚨 *¡ALERTA DE GAS/HUMO!* \nSe ha detectado una concentración elevada de gas en el ambiente.");
          if (enviado) {
            ultimoEnvioTelegram = millis();
          }
        }
      }
    }
    Serial.println("");

    delay(2000); 
  } 
  // Si NO hay Wi-Fi, activamos el protocolo de reconexión y parpadeo de error
  else {
    WiFi.reconnect();
    indicarErrorConexion();
  }
}

// Función que analiza si las últimas lecturas son estables
void verificarEstabilizacion(int valorActual) {
  ultimasLecturas[indiceLectura] = valorActual;
  indiceLectura = (indiceLectura + 1) % 10;

  if (indiceLectura == 0) {
    arregloLleno = true;
  }

  // Solo empezamos a evaluar cuando ya llenamos al menos una ronda de 10 lecturas (20 segundos)
  if (arregloLleno) {
    int minVal = ultimasLecturas[0];
    int maxVal = ultimasLecturas[0];

    for (int i = 1; i < 10; i++) {
      if (ultimasLecturas[i] < minVal) minVal = ultimasLecturas[i];
      if (ultimasLecturas[i] > maxVal) maxVal = ultimasLecturas[i];
    }

    int delta = maxVal - minVal;
    Serial.printf(" (Variación en 20s: %d)", delta);

    // Si la diferencia máxima entre las últimas 10 lecturas es menor o igual a 8 puntos, considéralo estable
    if (delta <= 8) {
      sensorEstabilizado = true;
      unsigned long tiempoTotalMinutos = (millis() - tiempoInicioEstabilizacion) / 60000;
      Serial.println("\n[+] ¡Sensor MQ-2 estabilizado con éxito!");
      
      String msg = "✅ *Sensor MQ-2 Estabilizado*\nEl sensor ha alcanzado su temperatura óptima de operación.\nValor base de aire limpio: " + String(valorActual) + "\nTiempo transcurrido: ~" + String(tiempoTotalMinutos) + " minutos.";
      enviarMensajeTelegram(msg);
    }
  }
}

// Función auxiliar para formatear caracteres especiales en la URL
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

// Función para enviar mensajes a Telegram vía HTTPS
bool enviarMensajeTelegram(String mensaje) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  String url = "/bot" + String(SECRET_BOT_TOKEN) + "/sendMessage?chat_id=" + String(SECRET_CHAT_ID) + "&text=" + urlEncode(mensaje) + "&parse_mode=Markdown";

  Serial.println("[Telegram] Enviando mensaje...");
  
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
          return true;
        }
      }
    }
    client.stop();
  }
  
  Serial.println("[Telegram] Error al enviar el mensaje.");
  return false;
}

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