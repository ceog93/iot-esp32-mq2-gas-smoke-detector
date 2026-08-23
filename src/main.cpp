#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"

// Pin del LED integrado
const int PIN_LED_BUILDIN = 2;

// Parámetros de conexión
const int MAX_INTENTOS_WIFI = 25;
bool wifiConectado = false;

void indicarConexionExitosa();
void indicarErrorConexion();

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PIN_LED_BUILDIN, OUTPUT);
  digitalWrite(PIN_LED_BUILDIN, LOW);

  Serial.println("\n==========================================");
  Serial.println("   INICIANDO CONEXIÓN WI-FI EN ESP32      ");
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
    Serial.println("\n[+] Conexión Wi-Fi establecida con éxito.");
    Serial.print("    Dirección IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf("    Calidad señal (RSSI): %d dBm\n", WiFi.RSSI());

    // 3 destellos rápidos de confirmación
    indicarConexionExitosa();
    
    // Apagar el LED definitivamente
    digitalWrite(PIN_LED_BUILDIN, LOW);
  } else {
    wifiConectado = false;
    Serial.println("\n[-] Error: No se pudo conectar a la red Wi-Fi.");
    digitalWrite(PIN_LED_BUILDIN, LOW);
  }
}

void loop() {
  if (wifiConectado) {
    // Verificamos el estado, pero damos un margen de reintento interno 
    // para evitar falsas desconexiones momentáneas.
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[!] Conexión perdida. Intentando reconectar...");
      
      // Intentamos reconectar de manera nativa
      WiFi.reconnect();
      delay(2000); // Esperar un momento prudente
      
      // Si tras el reintento sigue sin conectar, activamos el estado de error
      if (WiFi.status() != WL_CONNECTED) {
        wifiConectado = false;
      }
    }
    delay(2000); // Revisar el estado del Wi-Fi cada 2 segundos sin molestar al LED
  } else {
    // Estado Error real: Doble destello continuo
    indicarErrorConexion();
  }
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