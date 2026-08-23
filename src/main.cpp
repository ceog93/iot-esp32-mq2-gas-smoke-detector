#include <Arduino.h>

// El LED integrado suele estar en GPIO 2 (en algunas variantes NodeMCU está en GPIO 1)
const int PIN_LED_BUILDIN = 2; 

int contadorCiclos = 0;

void setup() {
  // Inicialización de la consola serial a 115200 baudios
  Serial.begin(115200);
  delay(1000);

  // Configuración del pin del LED integrado como salida digital
  pinMode(PIN_LED_BUILDIN, OUTPUT);

  Serial.println("\n==========================================");
  Serial.println("  PRUEBA DE HARDWARE: LED INTEGRADO ESP32 ");
  Serial.println("==========================================");
  Serial.println("Iniciando secuencia de parpadeo...");
}

void loop() {
  contadorCiclos++;

  // Encender LED integrado
  digitalWrite(PIN_LED_BUILDIN, HIGH);
  Serial.printf("[Ciclo %d] LED Integrado (GPIO %d): ENCENDIDO\n", contadorCiclos, PIN_LED_BUILDIN);
  delay(500);

  // Apagar LED integrado
  digitalWrite(PIN_LED_BUILDIN, LOW);
  Serial.printf("[Ciclo %d] LED Integrado (GPIO %d): APAGADO\n", contadorCiclos, PIN_LED_BUILDIN);
  delay(500);
}