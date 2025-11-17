#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
// --- CONFIG ---
const char* WIFI_SSID = "Fami-Castro";
const char* WIFI_PASS = "72225167";
const char* SERVER_URL = "https://servidormelendeziot-production.up.railway.app/api/telemetria/";

// --- API Comandos ---
const char* BASE_API = "https://servidormelendeziot-production.up.railway.app/api";
const char* API_TOKEN = "21c38d2f267b4b2933c47ac671220dc1ce159515"; // Authorization: Token <token>
const char* URL_COMANDOS_PEND = "/comandos/pendientes/";

// --- VARIABLES GLOBALES PARA ARDUINO ---
String color_detected = "NINGUNO";
float temperatura = 0.0;
bool datos_arduino_nuevos = false;

// --- OLED ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SDA 19
#define OLED_SCL 18
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- 7 SEGMENTOS ---
#define SEG_A 12
#define SEG_B 13
#define SEG_C 26
#define SEG_D 25
#define SEG_E 33
#define SEG_F 14
#define SEG_G 27

// --- SENSORES ---
#define TRIG 23
#define ECHO 22
#define BUZZER 15
#define VIBRADOR 5
#define RX2 16
#define TX2 17

// --- NÚMEROS 7 SEG ---
const bool numeros[10][7] = {
  {1,1,1,1,1,1,0}, {0,1,1,0,0,0,0}, {1,1,0,1,1,0,1}, {1,1,1,1,0,0,1},
  {0,1,1,0,0,1,1}, {1,0,1,1,0,1,1}, {1,0,1,1,1,1,1}, {1,1,1,0,0,0,0},
  {1,1,1,1,1,1,1}, {1,1,1,1,0,1,1}
};

#define PI 3.14159265359

// --- RANGO 40CM ---
#define LIMITE_DETECCION 40.0
#define ZONA_ALERTA 30.0
#define ZONA_PELIGRO 20.0
#define ZONA_CRITICA 10.0
#define ZONA_EMERGENCIA 5.0

// --- VARIABLES GLOBALES ---
float distancia_actual = 0.0;
int nivel_actual = 0;
int tipo_zona = 0;

// --- PARA ENVÍO EN HILO ---
struct DatosEnvio {
  float distancia;
  int nivel;
  String color;
  float temperatura;
};
DatosEnvio datos_para_enviar;
bool hay_datos_nuevos = false;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// --- VARIABLES DE ALERTAS ---
unsigned long tiempo_ultimo_beep = 0;
unsigned long tiempo_ultimo_vibrador = 0;

// --- HILOS ---
TaskHandle_t TaskEnvio;
TaskHandle_t TaskComandos;

// --- PULSO Y PARTÍCULAS ---
unsigned long ultimo_pulso = 0;
bool pulso_activo = false;
int radio_pulso = 0;
int x_objeto = 0, y_objeto = 0;
bool mostrar_objeto = false;
unsigned long tiempo_objeto = 0;
int particulas_x[8], particulas_y[8];
bool particulas_activas[8] = {false};

// --- ESTADO DE OLED: MENSAJE VS RADAR ---
bool radarSuspendido = false;
String mensajeOLED = "";
unsigned long mensajeDesde = 0;
// 0 = no auto-reanudar; si quieres auto volver al radar, pon ms (ej. 10000 para 10s)
const unsigned long MENSAJE_MAX_MS = 0;

// --- PROTOTIPOS ---
void tareaEnvioHTTP(void *pvParameters);
void tareaComandos(void *pv);
void procesarComandosPendientes();
void marcarComandoEjecutado(int id);
void aplicarComando(const String& comando, const String& parametros);
void dibujarMensajeOLED();

float medirDistancia();
int escalaDistancia(float d);
void controlarAlertasProximidad(float distancia, unsigned long ahora);
void controlar_pulso(unsigned long ahora);
void activarParticulas();
void actualizarParticulas();
void mostrarNumero(int n);
void dibujarRadarMilitar();

// =============================================================
// SETUP
// =============================================================
void setup() {
  Serial.begin(9600);
  Serial2.begin(9600, SERIAL_8N1, RX2, TX2);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED FALLÓ");
    while (1);
  }

  // Boot
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(32, 25);
  display.print("RADAR");
  display.setCursor(45, 38);
  display.print("ONLINE");
  display.display();
  delay(1000);

  int pines[] = {SEG_A, SEG_B, SEG_C, SEG_D, SEG_E, SEG_F, SEG_G};
  for (int i = 0; i < 7; i++) pinMode(pines[i], OUTPUT);
  pinMode(TRIG, OUTPUT); pinMode(ECHO, INPUT);
  pinMode(BUZZER, OUTPUT); pinMode(VIBRADOR, OUTPUT);

  mostrarNumero(0);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi OK");

  // === HILO DE ENVÍO ULTRA RÁPIDO ===
  xTaskCreatePinnedToCore(
    tareaEnvioHTTP,     // Función
    "EnvioHTTP",        // Nombre
    12000,              // Stack
    NULL,               // Parámetro
    2,                  // Prioridad
    &TaskEnvio,         // Handle
    1                   // Core 1
  );

  // === HILO DE COMANDOS ===
  xTaskCreatePinnedToCore(
    tareaComandos,
    "ComandosHTTP",
    12000,
    NULL,
    2,
    &TaskComandos,
    1
  );
}

// =============================================================
// LOOP PRINCIPAL
// =============================================================
void loop() {
  unsigned long ahora = millis();

  float nueva_distancia = medirDistancia();
  int nuevo_nivel = escalaDistancia(nueva_distancia);

  portENTER_CRITICAL(&mux);
  distancia_actual = nueva_distancia;
  nivel_actual = nuevo_nivel;
  hay_datos_nuevos = true;
  portEXIT_CRITICAL(&mux);

  controlarAlertasProximidad(nueva_distancia, ahora);
  mostrarNumero(nuevo_nivel);

  // === LEER DATOS DEL ARDUINO (Serial2) ===
  if (Serial2.available()) {
    String dato = Serial2.readStringUntil('\n');
    dato.trim();
    if (dato.length() == 0) return;

    Serial.println("ARD: " + dato);  // Depuración

    // --- PARSEAR AMBOS FORMATOS ---
    bool parsed = false;

    // Formato 1: R=142 G=157 B=132 -> NINGUNO | Temp: 38.3C
    if (dato.indexOf("R=") != -1 && dato.indexOf("G=") != -1 && dato.indexOf("B=") != -1) {
      int r = 0, g = 0, b = 0;
      sscanf(dato.c_str(), "R=%d G=%d B=%d", &r, &g, &b);

      // Detectar color
      if (r > 130 && g > 130 && b > 130) color_detected = "BLANCO";
      else if (r > g && r > b) color_detected = "ROJO";
      else if (g > r && g > b) color_detected = "VERDE";
      else if (b > r && b > g) color_detected = "AZUL";
      else color_detected = "NINGUNO";

      // Extraer temperatura
      int tempPos = dato.indexOf("Temp: ");
      if (tempPos != -1) {
        temperatura = dato.substring(tempPos + 6).toFloat();
      }
      parsed = true;
    }
    // Formato 2: NINGUNO,38.3
    else if (dato.indexOf("NINGUNO,") != -1) {
      color_detected = "NINGUNO";
      temperatura = dato.substring(dato.indexOf(",") + 1).toFloat();
      parsed = true;
    }

    if (parsed) {
      portENTER_CRITICAL(&mux);
      datos_arduino_nuevos = true;
      portEXIT_CRITICAL(&mux);
    }
  }

  controlar_pulso(ahora);

  // Mostrar según estado
  if (radarSuspendido) {
    // Apagar alertas físicas mientras se muestra mensaje
    noTone(BUZZER);
    digitalWrite(VIBRADOR, LOW);

    dibujarMensajeOLED();

    // Auto‑reanudar (si configuraste un tiempo)
    if (MENSAJE_MAX_MS > 0 && (millis() - mensajeDesde) > MENSAJE_MAX_MS) {
      radarSuspendido = false;
      mensajeOLED = "";
    }
  } else {
    dibujarRadarMilitar();
  }

  delay(25);  // ~40 FPS
}

// =============================================================
// HILO DE ENVÍO HTTP (CADA 200ms)
// =============================================================
void tareaEnvioHTTP(void *pvParameters) {
  DatosEnvio datos_local;
  unsigned long ultimo_envio = 0;
  const unsigned long INTERVALO_ENVIO = 200;

  for (;;) {
    portENTER_CRITICAL(&mux);
    if (hay_datos_nuevos) {
      datos_local.distancia = distancia_actual;
      datos_local.nivel = nivel_actual;
      hay_datos_nuevos = false;
    }
    if (datos_arduino_nuevos) {
      datos_local.color = color_detected;
      datos_local.temperatura = temperatura;
      datos_arduino_nuevos = false;
    }
    portEXIT_CRITICAL(&mux);

    unsigned long ahora = millis();

    if (ahora - ultimo_envio >= INTERVALO_ENVIO && datos_local.nivel > 0) {
      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.setConnectTimeout(1000);
        http.setTimeout(1000);
        http.begin(SERVER_URL);
        http.addHeader("Content-Type", "application/json");

        String json = "{"
          "\"distancia\":" + String(datos_local.distancia / 100.0, 2) +
          ",\"nivel\":" + String(datos_local.nivel) +
          ",\"color\":\"" + datos_local.color + "\"" +
          ",\"temperatura\":" + String(datos_local.temperatura, 1) +
        "}";

        int code = http.POST(json);
        if (code > 0) {
          Serial.printf("ENVIADO [N%d | %.1fcm | %s | %.1f°C]\n",
                        datos_local.nivel, datos_local.distancia,
                        datos_local.color.c_str(), datos_local.temperatura);
        } else {
          Serial.printf("ERROR HTTP: %d\n", code);
        }
        http.end();
      }
      ultimo_envio = ahora;
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// =============================================================
// HILO DE COMANDOS (CADA 1s)
// =============================================================
void tareaComandos(void *pv) {
  unsigned long ultimo = 0;
  const unsigned long INTERVALO = 1000; // 1s

  for (;;) {
    unsigned long ahora = millis();
    if (ahora - ultimo >= INTERVALO && WiFi.status() == WL_CONNECTED) {
      procesarComandosPendientes();
      ultimo = ahora;
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// =============================================================
// COMANDOS: descargar, aplicar y marcar ejecutados
// =============================================================
// VERSIÓN CON DEBUG MEJORADO - Reemplaza tu función procesarComandosPendientes() con esta:

void procesarComandosPendientes() {
  HTTPClient http;
  String url = String(BASE_API) + String(URL_COMANDOS_PEND);
  
  Serial.println("=== CONSULTANDO COMANDOS ===");
  Serial.println("URL: " + url);
  
  http.begin(url);
  http.addHeader("Authorization", String("Token ") + API_TOKEN);
  
  int code = http.GET();
  Serial.printf("GET comandos -> código: %d\n", code);

  if (code == 200) {
    String payload = http.getString();
    Serial.println("Payload recibido:");
    Serial.println(payload);
    Serial.println("---");

    payload.trim();
    if (payload.length() == 0 || payload == "[]") {
      Serial.println("No hay comandos pendientes");
      http.end();
      Serial.println("=== FIN CONSULTA ===\n");
      return;
    }

    DynamicJsonDocument doc(8192); // ajusta si la respuesta es muy grande
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      Serial.print("Error parseando JSON: ");
      Serial.println(err.c_str());
      http.end();
      Serial.println("=== FIN CONSULTA ===\n");
      return;
    }

    JsonArray comandos = doc.as<JsonArray>();
    int procesados = 0;

    for (JsonVariant item : comandos) {
      int id = item["id"] | -1;
      const char* comando = item["comando"] | "";
      const char* parametros = item["parametros"] | "";
      Serial.printf("Comando #%d -> id=%d, comando=%s, parametros=%s\n",
                    procesados + 1, id, comando, parametros);

      // Ejecutar en orden seguro
      if (strcmp(comando, "INTERRUPT_RADAR") == 0 || strcmp(comando, "RESUME_RADAR") == 0) {
        aplicarComando(comando, parametros);
      }
      if (strcmp(comando, "SHOW_MESSAGE") == 0) {
        aplicarComando(comando, parametros);
      }

      if (id >= 0) {
        Serial.printf("Marcando comando %d como ejecutado...\n", id);
        marcarComandoEjecutado(id);
      }
      procesados++;
    }

    Serial.printf("Total comandos procesados: %d\n", procesados);
  } else if (code == 401) {
    Serial.println("ERROR: Token inválido o no autorizado (401)");
  } else if (code == 404) {
    Serial.println("ERROR: Endpoint no encontrado (404)");
  } else {
    Serial.printf("ERROR: Código HTTP %d\n", code);
    Serial.println("Respuesta: " + http.getString());
  }

  http.end();
  Serial.println("=== FIN CONSULTA ===\n");
}


void marcarComandoEjecutado(int id) {
  HTTPClient http;
  String url = String(BASE_API) + "/comandos/" + String(id) + "/marcar_ejecutado/";
  http.begin(url);
  http.addHeader("Authorization", String("Token ") + API_TOKEN);
  int code = http.POST(""); // sin body
  // Serial.printf("Marcar ejecutado [%d] -> %d\n", id, code);
  http.end();
}

void aplicarComando(const String& comando, const String& parametros) {
  Serial.printf("aplicarComando: %s, params: %s\n", comando.c_str(), parametros.c_str());

  if (comando == "INTERRUPT_RADAR") {
    radarSuspendido = true;
    mensajeOLED = "";
    Serial.println(">>> RADAR SUSPENDIDO");
    return;
  }

  if (comando == "SHOW_MESSAGE") {
    radarSuspendido = true;
    mensajeOLED = parametros;
    mensajeDesde = millis();
    Serial.printf(">>> MOSTRANDO MENSAJE: %s\n", mensajeOLED.c_str());
    return;
  }

  if (comando == "RESUME_RADAR") {
    radarSuspendido = false;
    mensajeOLED = "";
    Serial.println(">>> RADAR REANUDADO");
    return;
  }

  // Si no coincide con ningún comando conocido
  Serial.printf(">>> COMANDO DESCONOCIDO: %s\n", comando.c_str());
}


// =============================================================
// FUNCIONES AUXILIARES
// =============================================================
float medirDistancia() {
  digitalWrite(TRIG, LOW); delayMicroseconds(2);
  digitalWrite(TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  
  long duracion = pulseIn(ECHO, HIGH, 60000);
  if (duracion == 0) return 999;
  
  float distancia = duracion * 0.034 / 2;
  if (distancia > LIMITE_DETECCION) return 999;
  return distancia;
}

int escalaDistancia(float d) {
  if (d > LIMITE_DETECCION || d <= 0) return 0;
  if (d < 4) d = 4;
  int nivel = 9 - ((d - 4) / 4);
  return constrain(nivel, 1, 9);
}

void controlarAlertasProximidad(float distancia, unsigned long ahora) {
  if (distancia > LIMITE_DETECCION || distancia <= 0) {
    tipo_zona = 0; noTone(BUZZER); digitalWrite(VIBRADOR, LOW); return;
  }
  
  if (distancia <= ZONA_EMERGENCIA) tipo_zona = 4;
  else if (distancia <= ZONA_CRITICA) tipo_zona = 3;
  else if (distancia <= ZONA_PELIGRO) tipo_zona = 2;
  else tipo_zona = 1;

  int intervalo = 0, freq = 0, dur = 0;
  switch(tipo_zona) {
    case 1: intervalo = 1400; freq = 500; dur = 70; digitalWrite(VIBRADOR, LOW); break;
    case 2: intervalo = 800;  freq = 1000; dur = 90; digitalWrite(VIBRADOR, LOW); break;
    case 3: intervalo = 450;  freq = 1700; dur = 110; digitalWrite(VIBRADOR, LOW); break;
    case 4: intervalo = 180;  freq = 2400; dur = 70;
      if (ahora - tiempo_ultimo_vibrador >= 180) {
        digitalWrite(VIBRADOR, !digitalRead(VIBRADOR));
        tiempo_ultimo_vibrador = ahora;
      }
      break;
  }
  
  if (ahora - tiempo_ultimo_beep >= intervalo) {
    tone(BUZZER, freq, dur);
    tiempo_ultimo_beep = ahora;
  }
}

void controlar_pulso(unsigned long ahora) {
  if (!pulso_activo && ahora - ultimo_pulso > 750) {
    pulso_activo = true;
    radio_pulso = 0;
    ultimo_pulso = ahora;
  }

  if (pulso_activo) {
    radio_pulso += 5;
    if (radio_pulso > 58) {
      pulso_activo = false;
      radio_pulso = 0;
    }

    if (distancia_actual > 0 && distancia_actual <= LIMITE_DETECCION) {
      int radio_obj = map((int)distancia_actual, 0, 40, 5, 55);
      if (radio_pulso >= radio_obj - 5 && radio_pulso <= radio_obj + 5) {
        mostrar_objeto = true;
        tiempo_objeto = ahora;
        float ang = PI / 4;
        x_objeto = 42 + radio_obj * cos(ang);
        y_objeto = 36 - radio_obj * sin(ang);
      }
    }
  }

  if (mostrar_objeto && ahora - tiempo_objeto > 1400) {
    mostrar_objeto = false;
  }
}

void activarParticulas() {
  for (int i = 0; i < 8; i++) {
    if (!particulas_activas[i]) {
      particulas_activas[i] = true;
      particulas_x[i] = x_objeto + random(-10, 11);
      particulas_y[i] = y_objeto + random(-10, 11);
      break;
    }
  }
}

void actualizarParticulas() {
  for (int i = 0; i < 8; i++) {
    if (particulas_activas[i]) {
      particulas_y[i] -= 4;
      particulas_x[i] += random(-4, 5);
      if (particulas_y[i] < 0 || particulas_x[i] < 0 || particulas_x[i] > 100) {
        particulas_activas[i] = false;
      }
    }
  }
}

void mostrarNumero(int n) {
  if (n < 0 || n > 9) n = 0;
  int pines[] = {SEG_A, SEG_B, SEG_C, SEG_D, SEG_E, SEG_F, SEG_G};
  for (int i = 0; i < 7; i++) digitalWrite(pines[i], numeros[n][i]);
}

// =============================================================
// RADAR MILITAR
// =============================================================
void dibujarRadarMilitar() {
  display.clearDisplay();

  int cx = 42, cy = 36, rmax = 55;
  display.drawCircle(cx, cy, 18, SSD1306_WHITE);
  display.drawCircle(cx, cy, 36, SSD1306_WHITE);
  display.drawCircle(cx, cy, rmax, SSD1306_WHITE);

  if (pulso_activo) {
    float ang = PI / 4;
    int x2 = cx + radio_pulso * cos(ang);
    int y2 = cy - radio_pulso * sin(ang);
    display.drawLine(cx, cy, x2, y2, SSD1306_WHITE);
    display.fillCircle(x2, y2, 2, SSD1306_WHITE);
  }

  if (mostrar_objeto && distancia_actual <= LIMITE_DETECCION) {
    int radio_obj = map((int)distancia_actual, 0, 40, 5, rmax);
    float ang = PI / 4;
    x_objeto = cx + radio_obj * cos(ang);
    y_objeto = cy - radio_obj * sin(ang);

    int pulso = (millis() / 70) % 12;
    int tam = 3 + (pulso > 6 ? 12 - pulso : pulso);
    if ((millis() % 160) < 90) {
      display.fillCircle(x_objeto, y_objeto, tam, SSD1306_WHITE);
      display.drawCircle(x_objeto, y_objeto, tam + 4, SSD1306_WHITE);
    }
    activarParticulas();
  }

  int col = 90;
  if (distancia_actual > 0 && distancia_actual <= LIMITE_DETECCION) {
    char buf[6];
    sprintf(buf, "%2d", (int)distancia_actual);
    display.setCursor(col, 8);
    display.print(buf);
    display.setCursor(col + 16, 8);
    display.print("cm");
  } else {
    display.setCursor(col, 8);
    display.print("--cm");
  }

  display.setCursor(col, 20);
  display.print("N");
  display.print(nivel_actual);

  int barra_h = map(constrain((int)distancia_actual, 0, 40), 0, 40, 0, 24);
  display.drawRect(col - 4, 8, 8, 26, SSD1306_WHITE);
  if (distancia_actual > 0 && distancia_actual <= 40) {
    display.fillRect(col - 3, 9 + (25 - barra_h), 6, barra_h, SSD1306_WHITE);
  }

  const char* zonas[] = {"", "ALERT", "DANGER", "CRITIC", "EMERG!"};
  if (tipo_zona > 0) {
    display.setCursor(col - 12, 52);
    if (tipo_zona >= 3 && (millis() % 350 < 175)) {
      display.fillRect(col - 14, 50, 56, 10, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    }
    display.print(zonas[tipo_zona]);
    display.setTextColor(SSD1306_WHITE);
  }

  actualizarParticulas();
  for (int i = 0; i < 8; i++) {
    if (particulas_activas[i]) {
      int px = constrain(particulas_x[i], 5, 100);
      int py = constrain(particulas_y[i], 5, 60);
      display.fillCircle(px, py, 1, SSD1306_WHITE);
    }
  }

  if (tipo_zona == 4 && (millis() % 250 < 125)) {
    display.drawRect(0, 0, 128, 64, SSD1306_WHITE);
  }

  display.display();
}

// =============================================================
// MENSAJE OLED
// =============================================================
void dibujarMensajeOLED() {
  static String  ultimoMensaje = "";
  static String  lineas[12];
  static int     totalLineas = 0;
  static int     segmentoActual = 0;
  static int     totalSegmentos = 1;
  static unsigned long ultimoCambio = 0;

  String texto = mensajeOLED;
  texto.trim();
  if (texto.isEmpty()) {
    texto = "Mensaje";
  }

  const uint8_t charsPorLinea = 10;       // tamaño 2, ~10 caracteres útiles
  const uint8_t lineasPorSegmento = 3;    // máximo de líneas visibles por “pantalla”
  const uint8_t textSize = 2;
  const uint8_t anchoCar = 6 * textSize;
  const uint8_t altoLinea = 8 * textSize + 2;  // 18 px

  if (texto != ultimoMensaje) {
    ultimoMensaje = texto;
    totalLineas = 0;
    segmentoActual = 0;
    ultimoCambio = millis();

    int idx = 0;
    int len = texto.length();
    while (idx < len && totalLineas < 12) {
      int end = idx + charsPorLinea;
      if (end > len) end = len;

      if (end < len) {
        int space = texto.lastIndexOf(' ', end - 1);
        if (space >= idx) {
          end = space;
        }
      }
      if (end == idx) {
        end = idx + charsPorLinea;
        if (end > len) end = len;
      }

      String linea = texto.substring(idx, end);
      linea.trim();
      if (!linea.isEmpty()) {
        lineas[totalLineas++] = linea;
      }

      idx = end;
      while (idx < len && texto.charAt(idx) == ' ') idx++;
    }

    if (totalLineas == 0) {
      lineas[totalLineas++] = texto.substring(0, min(len, (int)charsPorLinea));
    }

    totalSegmentos = (totalLineas + lineasPorSegmento - 1) / lineasPorSegmento;
  }

  if (totalSegmentos > 1 && millis() - ultimoCambio > 3000) {
    segmentoActual = (segmentoActual + 1) % totalSegmentos;
    ultimoCambio = millis();
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(textSize);

  int primeraLinea = segmentoActual * lineasPorSegmento;
  int ultimaLinea = min(primeraLinea + lineasPorSegmento, totalLineas);
  int lineasVisible = ultimaLinea - primeraLinea;

  int altoTotal = lineasVisible * altoLinea;
  int y = (SCREEN_HEIGHT - altoTotal) / 2;
  if (y < 0) y = 0;

  for (int i = primeraLinea; i < ultimaLinea; ++i) {
    String linea = lineas[i];
    int anchoLinea = linea.length() * anchoCar;
    int x = (SCREEN_WIDTH - anchoLinea) / 2;
    if (x < 0) x = 0;
    display.setCursor(x, y);
    display.println(linea);
    y += altoLinea;
  }

  if (totalSegmentos > 1) {
    int dotY = SCREEN_HEIGHT - 6;
    int dotSpacing = 6;
    int totalDotsWidth = (totalSegmentos - 1) * dotSpacing;
    int dotX = (SCREEN_WIDTH - totalDotsWidth) / 2;

    for (int i = 0; i < totalSegmentos; ++i) {
      int radius = (i == segmentoActual) ? 2 : 1;
      display.fillCircle(dotX + i * dotSpacing, dotY, radius, SSD1306_WHITE);
    }
  }

  display.display();
}