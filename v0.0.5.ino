#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

// Credenciales de acceso a red WiFi
const char* ssid = "Verónica PRIME";
const char* password = "veritowifi";

// Configura los parámetros de tu bot de Telegram
#define BOTtoken "8003846621:AAFlUkgrSASUBH0rW4dKs77CIzLg0Dhua3I"  // Token de tu bot
#define CHAT_ID "7921063431"  // Tu ID de chat de Telegram

// Configuración del sensor de humedad de suelo
#define SENSOR_ANALOG A0      // Pin analógico para leer valor del sensor
#define SENSOR_DIGITAL 4      // GPIO4 (equivalente a D2 en NodeMCU)
#define PIN_BOMBA 0          // GPIO0 (D3 en NodeMCU) para controlar la bomba

// Configuración de la pantalla LCD I2C
// Dirección típica 0x27 o 0x3F - puedes encontrarla usando un scanner I2C
// Formato: dirección, columnas, filas
LiquidCrystal_I2C lcd(0x27, 16, 2);  // Cambia a 0x3F si no funciona

// Valores de calibración para el sensor (ajustados según tus mediciones)
int valorSeco = 1023;
int valorMojado = 0;    // Valor cuando el sensor está en agua (mojado)

// Umbrales y configuraciones
#define HUMEDAD_BAJA 30.0     // Alerta cuando humedad < 30%
int umbralMinHumedad = 30;
int umbralMaxHumedad = 70;
#define CAUDAL_BOMBA 20       // ml por segundo - ¡DEBES CALIBRAR ESTE VALOR!
#define RIEGO_DEFAULT 100     // Cantidad default de ml para regar
#define ML_RIEGO_AUTO 50      // ml a dispensar en cada riego automático
#define INTERVALO_NOTIF 43200000 // 12 horas en milisegundos para notificaciones
#define MAX_REINTENTOS_WIFI 20   // Máximo número de intentos para conectar WiFi

// Variables para el tiempo
unsigned long lastTimeBotRan;
const unsigned long BOT_MTBS = 1000;  // Tiempo entre escaneos de mensajes Telegram
unsigned long ultimaLecturaSensor = 0;
const unsigned long INTERVALO_LECTURA = 5000;  // Leer sensor cada 5 segundos
unsigned long ultimaActualizacionLCD = 0;
const unsigned long INTERVALO_LCD = 500;       // Actualizar LCD cada 0.5 segundos
unsigned long ultimoGuardadoEEPROM = 0;
const unsigned long INTERVALO_GUARDADO = 600000; // Guardar estado cada 10 minutos

// Variables para bomba y riego
bool bombaTrabajando = false;
unsigned long tiempoInicioBomba = 0;
int mlObjetivo = 0;
unsigned long tiempoUltimoRiego = 0;  // Timestamp del último riego
bool plantaRegadaAlgunaVez = false;   // Flag para saber si la planta ha sido regada

// Variables para modo viaje
bool modoViajeActivo = false;
unsigned long ultimaNotificacion = 0;

// Variables para alertas
bool alertaEnviada = false;   // Para evitar múltiples alertas

X509List cert(TELEGRAM_CERTIFICATE_ROOT);
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

// Variables para almacenar los últimos valores leídos
int ultimaHumedad = 0;
String ultimoEstadoDigital = "";
int intentosConexionFallidos = 0;
int calidadSenal = 0;

// Estructura para guardar en EEPROM
struct DatosGuardados {
  int humedad;
  bool modoViaje;
  int umbralMin;
  int umbralMax;
  bool regada;
  unsigned long tiempoRiego;
};

// Función para guardar datos en EEPROM
void guardarEstado() {
  DatosGuardados datos;
  datos.humedad = ultimaHumedad;
  datos.modoViaje = modoViajeActivo;
  datos.umbralMin = umbralMinHumedad;
  datos.umbralMax = umbralMaxHumedad;
  datos.regada = plantaRegadaAlgunaVez;
  datos.tiempoRiego = tiempoUltimoRiego;
  
  EEPROM.begin(sizeof(DatosGuardados));
  EEPROM.put(0, datos);
  EEPROM.commit();
  Serial.println("Estado guardado en EEPROM");
}

// Función para recuperar datos de EEPROM
void recuperarEstado() {
  DatosGuardados datos;
  
  EEPROM.begin(sizeof(DatosGuardados));
  EEPROM.get(0, datos);
  
  // Verificar que los datos sean válidos
  if (datos.humedad >= 0 && datos.humedad <= 100) {
    ultimaHumedad = datos.humedad;
    modoViajeActivo = datos.modoViaje;
    umbralMinHumedad = datos.umbralMin;
    umbralMaxHumedad = datos.umbralMax;
    plantaRegadaAlgunaVez = datos.regada;
    tiempoUltimoRiego = datos.tiempoRiego;
    Serial.println("Estado recuperado de EEPROM");
  } else {
    Serial.println("No hay datos válidos en EEPROM");
  }
}

// Función para enviar mensajes con reintentos
bool enviarMensajeTelegram(String chat_id, String mensaje) {
  int intentos = 0;
  bool envioExitoso = false;
  
  while (intentos < 3 && !envioExitoso) {
    // Intenta enviar el mensaje
    envioExitoso = bot.sendMessage(chat_id, mensaje, "");
    
    if (!envioExitoso) {
      intentos++;
      delay(1000);
    }
  }
  
  if (!envioExitoso) {
    Serial.println("Error al enviar mensaje a Telegram");
  }
  
  return envioExitoso;
}

void setup() {
  // Habilitar watchdog para reiniciar en caso de bloqueo
  ESP.wdtEnable(WDTO_8S);
  
  Serial.begin(115200);
  Serial.println("Iniciando sistema de monitoreo de humedad del suelo...");
  
  pinMode(SENSOR_DIGITAL, INPUT);  // Para lectura digital
  pinMode(PIN_BOMBA, OUTPUT);      // Para controlar la bomba
  digitalWrite(PIN_BOMBA, LOW);    // Asegurar que la bomba esté apagada al inicio
  
  // Inicializar pantalla LCD
  Wire.begin(4, 5);  // SDA = GPIO4 (D2), SCL = GPIO5 (D1)
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sistema riego");
  lcd.setCursor(0, 1);
  lcd.print("Conectando WiFi");
  
  // Configurar certificado para API de Telegram
  configTime(0, 0, "pool.ntp.org");
  client.setTrustAnchors(&cert);
  
  // Recuperar estado guardado
  recuperarEstado();
  
  // Conectar a WiFi
  conectarWifi();
  
  // Mostrar pantalla de bienvenida
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sistema de Riego");
  lcd.setCursor(0, 1);
  lcd.print("Automatizado");
  delay(2000);
  
  // Inicialización explícita de variables de tiempo
  ultimaLecturaSensor = 0;
  ultimaActualizacionLCD = 0;
  lastTimeBotRan = 0;
  ultimoGuardadoEEPROM = 0;
  
  // Leer sensor al iniciar
  leerSensor();
  
  // Mandar un mensaje al iniciar el bot
  if(CHAT_ID != "TU_ID_DE_CHAT_AQUÍ") {
    String mensaje = "✅ Sistema de monitoreo de humedad iniciado.\n";
    mensaje += "💧 Humedad actual: " + String(ultimaHumedad) + "%\n";
    mensaje += "📶 Calidad de señal WiFi: " + String(calidadSenal) + "%\n";
    mensaje += "Usa /ayuda para ver los comandos disponibles.";
    enviarMensajeTelegram(CHAT_ID, mensaje);
  } else {
    Serial.println("¡ADVERTENCIA! Debes cambiar CHAT_ID por tu propio ID de chat de Telegram");
  }
}

// Función para conectar a WiFi con reintentos
void conectarWifi() {
  WiFi.begin(ssid, password);
  Serial.print("Conectando a WiFi");
  int intentos = 0;
  
  while (WiFi.status() != WL_CONNECTED && intentos < MAX_REINTENTOS_WIFI) {
    Serial.print(".");
    lcd.setCursor(intentos % 13, 1);
    lcd.print(".");
    delay(500);
    intentos++;
    ESP.wdtFeed();  // Alimentar el watchdog durante la conexión
  }
  if (WiFi.status() == WL_CONNECTED) {
    intentosConexionFallidos = 0;
    Serial.println("");
    Serial.println("WiFi conectado");
    Serial.print("Dirección IP: ");
    Serial.println(WiFi.localIP());
    
    // Calcular calidad de señal WiFi (RSSI)
    long rssi = WiFi.RSSI();
    calidadSenal = constrain(map(rssi, -100, -50, 0, 100), 0, 100);
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi conectado!");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    delay(2000);
  } else {
    intentosConexionFallidos++;
    Serial.println("No se pudo conectar al WiFi");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Error WiFi");
    lcd.setCursor(0, 1);
    lcd.print("Reintento: " + String(intentosConexionFallidos));
    delay(3000);
    
    if (intentosConexionFallidos > 5) {
      Serial.println("Demasiados fallos de conexión. Reiniciando...");
      ESP.restart();
    }
  }
}

// Función para leer el sensor de humedad de suelo
void leerSensor() {
  // Alimentar el watchdog
  ESP.wdtFeed();
  
  // Imprimir mensaje de depuración
  Serial.println("Leyendo sensor de humedad...");
  
  // Leer valor analógico
  int valorAnalogico = analogRead(SENSOR_ANALOG);
  
  // Verificar límites del valor analógico
  if (valorAnalogico < 0 || valorAnalogico > 1024) {
    Serial.println("Error: Lectura del sensor fuera de rango");
    return;
  }
  
  // Imprimir valor crudo para depuración
  Serial.print("Valor analógico crudo: ");
  Serial.println(valorAnalogico);
  
  // Convertir a porcentaje (0% = seco, 100% = mojado)
  int humedadPorcentaje = map(valorAnalogico, valorSeco, valorMojado, 0, 100);
  
  // Limitar el porcentaje entre 0 y 100
  humedadPorcentaje = constrain(humedadPorcentaje, 0, 100);
  
  // Leer valor digital (opcional, para referencia)
  int valorDigital = digitalRead(SENSOR_DIGITAL);
  
  // AJUSTADO: Usar el porcentaje de humedad para determinar el estado
  String estadoDigital = (humedadPorcentaje < 50) ? "SECO" : "HÚMEDO";
  
  // Guardar los valores para mostrarlos en el LCD
  ultimaHumedad = humedadPorcentaje;
  ultimoEstadoDigital = estadoDigital;
  
  Serial.print("Humedad del suelo: ");
  Serial.print(humedadPorcentaje);
  Serial.print("% (Valor analógico: ");
  Serial.print(valorAnalogico);
  Serial.print(", Estado calculado: ");
  Serial.print(estadoDigital);
  Serial.println(")");
  
  // Verificar si humedad está por debajo del umbral
  if (humedadPorcentaje < HUMEDAD_BAJA && !alertaEnviada) {
    String mensaje = "⚠️ ALERTA: La humedad del suelo está baja (" + String(humedadPorcentaje) + "%)";
    enviarMensajeTelegram(CHAT_ID, mensaje);
    alertaEnviada = true;
    Serial.println("Alerta de humedad baja enviada");
  } 
  // Restablecer la bandera de alerta si la humedad vuelve a niveles normales
  else if (humedadPorcentaje >= HUMEDAD_BAJA && alertaEnviada) {
    String mensaje = "✅ La humedad del suelo ha vuelto a niveles normales (" + String(humedadPorcentaje) + "%)";
    enviarMensajeTelegram(CHAT_ID, mensaje);
    alertaEnviada = false;
    Serial.println("Mensaje de normalización enviado");
  }
  
  // Forzar actualización del LCD después de leer el sensor
  actualizarLCD();
  ultimaActualizacionLCD = millis();
}

// Función para actualizar la pantalla LCD
void actualizarLCD() {
  // Añadir depuración
  Serial.println("Actualizando LCD...");
  Serial.print("Mostrando humedad: ");
  Serial.println(ultimaHumedad);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Humedad: ");
  lcd.print(ultimaHumedad);
  lcd.print("%");
  
  lcd.setCursor(0, 1);
  // Mostrar emoji según el nivel de humedad
  if (ultimaHumedad < HUMEDAD_BAJA) {
    lcd.print("Estado: SECO!");
  } else {
    lcd.print("Estado: OK");
  }
  
  // Si la bomba está activa, mostrar un indicador
  if (bombaTrabajando) {
    lcd.setCursor(15, 0);
    lcd.print("*");
  }
  
  // Si el modo viaje está activo, mostrar indicador
  if (modoViajeActivo) {
    lcd.setCursor(15, 1);
    lcd.print("V");
  }
  
  Serial.println("LCD actualizado");
}

// Función para activar la bomba de agua con volumen en mililitros
void activarBomba(int mililitros = RIEGO_DEFAULT) {
  // Calcular duración en milisegundos basado en caudal
  int duracionMs = (mililitros * 1000) / CAUDAL_BOMBA;
  
  Serial.println("Activando bomba de agua...");
  Serial.print("Dispensando ");
  Serial.print(mililitros);
  Serial.print(" ml (");
  Serial.print(duracionMs);
  Serial.println(" ms)");
  
  digitalWrite(PIN_BOMBA, HIGH);
  bombaTrabajando = true;
  tiempoInicioBomba = millis();
  mlObjetivo = mililitros;
  
  // Guardar timestamp del riego
  tiempoUltimoRiego = millis();
  plantaRegadaAlgunaVez = true;
  
  // Mostrar en LCD
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Bomba ACTIVADA");
  lcd.setCursor(0, 1);
  lcd.print("Dispensando: " + String(mililitros) + "ml");
  
  // Enviar mensaje a Telegram
  enviarMensajeTelegram(CHAT_ID, "🚿 Bomba de agua ACTIVADA\nDispensando " + String(mililitros) + " ml de agua");
  
  // Guardar el estado actual en EEPROM
  guardarEstado();
}

// Función para desactivar la bomba de agua
void desactivarBomba() {
  Serial.println("Desactivando bomba de agua...");
  digitalWrite(PIN_BOMBA, LOW);
  bombaTrabajando = false;
  
  // Mostrar en LCD
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Bomba DESACTIVADA");
  
  // Enviar mensaje a Telegram
  enviarMensajeTelegram(CHAT_ID, "🚱 Bomba de agua DESACTIVADA");
  
  // Volver a mostrar estado normal después de 2 segundos
  delay(2000);
  actualizarLCD();
  
  // Guardar el estado actual en EEPROM
  guardarEstado();
}

// Función para convertir milisegundos a formato legible
String tiempoDesdeRiego() {
  if (!plantaRegadaAlgunaVez) {
    return "Nunca regada desde el inicio del sistema";
  }
  
  unsigned long tiempoActual = millis();
  unsigned long diferencia = tiempoActual - tiempoUltimoRiego;
  
  // Convertir a unidades de tiempo
  unsigned long segundos = diferencia / 1000;
  unsigned long minutos = segundos / 60;
  unsigned long horas = minutos / 60;
  unsigned long dias = horas / 24;
  
  segundos %= 60;
  minutos %= 60;
  horas %= 24;
  
  String resultado = "";
  
  if (dias > 0) {
    resultado += String(dias) + " día(s) ";
  }
  
  if (horas > 0 || dias > 0) {
    resultado += String(horas) + " hora(s) ";
  }
  
  if (minutos > 0 || horas > 0 || dias > 0) {
    resultado += String(minutos) + " minuto(s) ";
  }
  
  resultado += String(segundos) + " segundo(s)";
  
  return resultado;
}

// Función para activar el modo viaje
void activarModoViaje(int umbralMin = umbralMinHumedad, int umbralMax = umbralMaxHumedad) {
  modoViajeActivo = true;
  
  // Guardar preferencias
  umbralMinHumedad = umbralMin;
  umbralMaxHumedad = umbralMax;
  
  String mensaje = "✈️ Modo viaje ACTIVADO\n";
  mensaje += "La planta será regada automáticamente cuando la humedad baje del " + String(umbralMinHumedad) + "%\n";
  mensaje += "Recibirás notificaciones cada 12 horas sobre el estado.";
  
  enviarMensajeTelegram(CHAT_ID, mensaje);
  
  // Mostrar en LCD
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Modo Viaje: ON");
  lcd.setCursor(0, 1);
  lcd.print("Umbral: " + String(umbralMinHumedad) + "%");
  
  delay(2000);
  actualizarLCD();
  
  // Establecer el tiempo de la primera notificación
  ultimaNotificacion = millis();
  
  // Guardar estado en EEPROM
  guardarEstado();
}

// Función para desactivar el modo viaje
void desactivarModoViaje() {
  modoViajeActivo = false;
  
  String mensaje = "🏠 Modo viaje DESACTIVADO\n";
  mensaje += "El sistema vuelve al modo manual.";
  
  enviarMensajeTelegram(CHAT_ID, mensaje);
  
  // Mostrar en LCD
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Modo Viaje: OFF");
  
  delay(2000);
  actualizarLCD();
  
  // Guardar estado en EEPROM
  guardarEstado();
}

// Función para verificar si se necesita riego automático
void verificarRiegoAutomatico() {
  if (!modoViajeActivo || bombaTrabajando) return;
  
  // Si la humedad está por debajo del umbral, activar el riego
  if (ultimaHumedad < umbralMinHumedad) {
    String mensaje = "🤖 Riego automático activado\n";
    mensaje += "Humedad actual: " + String(ultimaHumedad) + "% (por debajo del umbral de " + String(umbralMinHumedad) + "%)\n";
    mensaje += "Dispensando " + String(ML_RIEGO_AUTO) + " ml de agua";
    
    enviarMensajeTelegram(CHAT_ID, mensaje);
    activarBomba(ML_RIEGO_AUTO);
  }
  
  // Enviar notificación periódica si corresponde
  unsigned long tiempoActual = millis();
  if (tiempoActual - ultimaNotificacion > INTERVALO_NOTIF) {
    String mensaje = "📊 Informe periódico de estado\n";
    mensaje += "Humedad actual: " + String(ultimaHumedad) + "%\n";
    mensaje += "Umbral mínimo: " + String(umbralMinHumedad) + "%\n";
    mensaje += "Último riego: hace " + tiempoDesdeRiego() + "\n";
    mensaje += "Modo viaje: ACTIVO";
    
    enviarMensajeTelegram(CHAT_ID, mensaje);
    ultimaNotificacion = tiempoActual;
  }
}

// Función para calibrar el sensor de humedad
void calibrarSensor(int valorSeco, int valorMojado) {
  valorSeco = valorSeco;
  valorMojado = valorMojado;
  
  String mensaje = "🔧 Sensor calibrado:\n";
  mensaje += "Valor en seco: " + String(valorSeco) + "\n";
  mensaje += "Valor en mojado: " + String(valorMojado);
  
  enviarMensajeTelegram(CHAT_ID, mensaje);
  
  // Guardar en EEPROM los valores de calibración
  // Aquí se podría extender para guardar estos valores también
  
  // Forzar una nueva lectura con los valores calibrados
  leerSensor();
}

void handleNewMessages(int numNewMessages) {
  Serial.print("Manejando ");
  Serial.print(numNewMessages);
  Serial.println(" mensajes nuevos");
  
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;
    
    // Imprimir en el monitor serie los mensajes recibidos para depuración
    Serial.print("Chat ID: ");
    Serial.println(chat_id);
    Serial.print("Mensaje de: ");
    Serial.println(from_name);
    Serial.print("Texto: ");
    Serial.println(text);
    
    // Comando /start o /ayuda
    if (text == "/start" || text == "/ayuda") {
      String welcome = "¡Hola, " + from_name + "! Soy tu sistema de monitoreo de humedad del suelo.\n";
      welcome += "Te notificaré cuando la humedad sea menor a " + String(HUMEDAD_BAJA) + "%.\n\n";
      welcome += "Comandos disponibles:\n";
      welcome += "/ayuda : Muestra este mensaje\n";
      welcome += "/humedad : Muestra la humedad actual del suelo\n";
      welcome += "/estado : Muestra información completa del sensor\n";
      welcome += "/regar : Activa el riego para dispensar " + String(RIEGO_DEFAULT) + " ml\n";
      welcome += "/regar_XX : Activa el riego para dispensar XX ml (máx. 500)\n";
      welcome += "/parar : Detiene el riego manualmente\n";
      welcome += "/modoviajeON : Activa el modo viaje con valores predeterminados\n";
      welcome += "/modoviajeON_min_max : Activa con umbrales personalizados\n";
      welcome += "/modoviajeOFF : Desactiva el modo viaje\n";
      welcome += "/modoviajeEstado : Muestra el estado del modo viaje\n";
      welcome += "/wifi : Muestra información de la conexión WiFi\n";
      
      enviarMensajeTelegram(chat_id, welcome);
      Serial.println("Mensaje de bienvenida enviado");
      
      // Mostrar en el LCD que se recibió un comando
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Comando /ayuda");
      lcd.setCursor(0, 1);
      lcd.print("De: " + from_name);
      delay(2000);
      actualizarLCD();
    }
    
    // Comando /humedad
    if (text == "/humedad") {
      // Tomar lectura fresca para dar el valor más actual
      leerSensor();
      
      String mensaje = "💧 La humedad del suelo actual es: " + String(ultimaHumedad) + "%";
      enviarMensajeTelegram(chat_id, mensaje);
      Serial.println("Información de humedad enviada");
      
      // Mostrar en el LCD que se recibió un comando
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Comando /humedad");
      lcd.setCursor(0, 1);
      lcd.print("De: " + from_name);
      delay(2000);
      actualizarLCD();
    }
    
    // Comando /estado
    if (text == "/estado") {
      // Tomar lectura fresca
      leerSensor();
      
      String mensaje = "Estado actual del suelo:\n";
      mensaje += "💧 Humedad: " + String(ultimaHumedad) + "%\n";
      mensaje += "🔄 Estado: " + ultimoEstadoDigital + "\n";
      mensaje += "🔔 Umbral de alerta: " + String(HUMEDAD_BAJA) + "%\n";
      mensaje += "🚿 Último riego: hace " + tiempoDesdeRiego() + "\n";
      
      if (modoViajeActivo) {
        mensaje += "✈️ Modo viaje: ACTIVO\n";
        mensaje += "⬇️ Umbral mínimo: " + String(umbralMinHumedad) + "%\n";
        mensaje += "⬆️ Umbral máximo: " + String(umbralMaxHumedad) + "%\n";
      } else {
        mensaje += "✈️ Modo viaje: INACTIVO\n";
      }
      
      mensaje += "📶 Calidad WiFi: " + String(calidadSenal) + "%";
      
      enviarMensajeTelegram(chat_id, mensaje);
      Serial.println("Información de estado enviada");
      
      // Mostrar en el LCD que se recibió un comando
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Comando /estado");
      lcd.setCursor(0, 1);
      lcd.print("De: " + from_name);
      delay(2000);
      actualizarLCD();
    }
    
    // Comando /regar
    if (text == "/regar") {
      activarBomba(RIEGO_DEFAULT);
      
      String mensaje = "🚿 Activando sistema de riego para dispensar " + String(RIEGO_DEFAULT) + " ml";
      enviarMensajeTelegram(chat_id, mensaje);
      
      // Mostrar en el LCD que se recibió un comando
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Comando /regar");
      lcd.setCursor(0, 1);
      lcd.print("De: " + from_name);
    }
    
// Comando /regar_ml para especificar volumen personalizado
    if (text.startsWith("/regar_")) {
      // Extraer mililitros del comando (formato: /regar_XX donde XX es ml)
      String mlStr = text.substring(7);
      int ml = mlStr.toInt();
      
      if (ml > 0 && ml <= 500) { // Limitar a máximo 500 ml por seguridad
        activarBomba(ml);
        
        String mensaje = "🚿 Activando sistema de riego para dispensar " + String(ml) + " ml";
        enviarMensajeTelegram(chat_id, mensaje);
      } else {
        enviarMensajeTelegram(chat_id, "⚠️ Cantidad inválida. Por favor usa un valor entre 1 y 500 ml.");
      }
    }
    
    // Comando /parar
    if (text == "/parar") {
      if (bombaTrabajando) {
        desactivarBomba();
        enviarMensajeTelegram(chat_id, "🛑 Sistema de riego detenido manualmente");
      } else {
        enviarMensajeTelegram(chat_id, "ℹ️ El sistema de riego no está activo actualmente");
      }
    }
    
    // Comando /modoviajeON
    if (text == "/modoviajeON") {
      activarModoViaje();
    }
    
    // Comando /modoviajeON con parámetros de umbrales
    if (text.startsWith("/modoviajeON_")) {
      // Extraer umbrales del comando (formato: /modoviajeON_min_max)
      String parametros = text.substring(13);
      int posGuion = parametros.indexOf('_');
      
      if (posGuion > 0) {
        String minStr = parametros.substring(0, posGuion);
        String maxStr = parametros.substring(posGuion + 1);
        
        int minHumedad = minStr.toInt();
        int maxHumedad = maxStr.toInt();
        
        if (minHumedad > 0 && minHumedad < maxHumedad && maxHumedad <= 100) {
          activarModoViaje(minHumedad, maxHumedad);
        } else {
          enviarMensajeTelegram(chat_id, "⚠️ Valores inválidos. El formato correcto es /modoviajeON_min_max donde min < max y ambos entre 1-100%");
        }
      } else {
        enviarMensajeTelegram(chat_id, "⚠️ Formato incorrecto. El formato correcto es /modoviajeON_min_max (ejemplo: /modoviajeON_30_70)");
      }
    }
    
    // Comando /modoviajeOFF
    if (text == "/modoviajeOFF") {
      desactivarModoViaje();
    }
    
    // Comando /modoviajeEstado
    if (text == "/modoviajeEstado") {
      String mensaje = "Estado del modo viaje:\n";
      if (modoViajeActivo) {
        mensaje += "✅ Modo viaje: ACTIVADO\n";
        mensaje += "⬇️ Umbral mínimo: " + String(umbralMinHumedad) + "%\n";
        mensaje += "⬆️ Umbral máximo: " + String(umbralMaxHumedad) + "%\n";
        mensaje += "🚿 Se dispensarán " + String(ML_RIEGO_AUTO) + " ml cuando la humedad baje del " + String(umbralMinHumedad) + "%\n";
        mensaje += "📩 Notificaciones cada " + String(INTERVALO_NOTIF / 3600000) + " horas";
      } else {
        mensaje += "❌ Modo viaje: DESACTIVADO\n";
        mensaje += "Utiliza /modoviajeON para activarlo con valores predeterminados\n";
        mensaje += "o /modoviajeON_min_max para umbrales personalizados";
      }
      
      enviarMensajeTelegram(chat_id, mensaje);
    }
    
    // Comando /wifi
    if (text == "/wifi") {
      // Verificar si estamos conectados al WiFi
      if (WiFi.status() == WL_CONNECTED) {
        // Actualizar calidad de señal
        long rssi = WiFi.RSSI();
        calidadSenal = constrain(map(rssi, -100, -50, 0, 100), 0, 100);
        
        String mensaje = "Información de la conexión WiFi:\n";
        mensaje += "📶 SSID: " + String(ssid) + "\n";
        mensaje += "📡 Dirección IP: " + WiFi.localIP().toString() + "\n";
        mensaje += "📊 Calidad de señal: " + String(calidadSenal) + "%\n";
        mensaje += "📏 RSSI: " + String(rssi) + " dBm\n";
        mensaje += "🔌 MAC: " + WiFi.macAddress();
        
        enviarMensajeTelegram(chat_id, mensaje);
      } else {
        enviarMensajeTelegram(chat_id, "⚠️ WiFi desconectado. Intentando reconectar...");
        conectarWifi();
      }
    }
    
    // Comando de calibración oculto (solo para desarrolladores)
    if (text.startsWith("/calibrar_")) {
      // Formato: /calibrar_valorSeco_valorMojado
      String parametros = text.substring(10);
      int posGuion = parametros.indexOf('_');
      
      if (posGuion > 0) {
        String secoStr = parametros.substring(0, posGuion);
        String mojadoStr = parametros.substring(posGuion + 1);
        
        int valorSeco = secoStr.toInt();
        int valorMojado = mojadoStr.toInt();
        
        if (valorSeco > valorMojado && valorSeco <= 1024 && valorMojado >= 0) {
          calibrarSensor(valorSeco, valorMojado);
        } else {
          enviarMensajeTelegram(chat_id, "⚠️ Valores de calibración inválidos. El valor en seco debe ser mayor que el mojado");
        }
      } else {
        enviarMensajeTelegram(chat_id, "⚠️ Formato incorrecto. El formato es /calibrar_valorSeco_valorMojado");
      }
    }
    
    // Comando para reiniciar manualmente el dispositivo
    if (text == "/reiniciar") {
      enviarMensajeTelegram(chat_id, "🔄 Reiniciando el sistema...");
      delay(1000);
      ESP.restart();
    }
    
    // Si el comando no es reconocido
    if (text.startsWith("/") && 
        !text.startsWith("/start") && 
        !text.startsWith("/ayuda") && 
        !text.startsWith("/humedad") && 
        !text.startsWith("/estado") && 
        !text.startsWith("/regar") && 
        !text.startsWith("/parar") && 
        !text.startsWith("/modoviajeON") && 
        !text.startsWith("/modoviajeOFF") && 
        !text.startsWith("/modoviajeEstado") && 
        !text.startsWith("/wifi") && 
        !text.startsWith("/calibrar_") &&
        !text.startsWith("/reiniciar")) {
        
        String mensaje = "⚠️ Comando no reconocido: " + text + "\n";
        mensaje += "Usa /ayuda para ver los comandos disponibles.";
        enviarMensajeTelegram(chat_id, mensaje);
    }
  }
}

// Función para verificar la conexión WiFi y reconectar si es necesario
void verificarWifi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Conexión WiFi perdida. Intentando reconectar...");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi perdido!");
    lcd.setCursor(0, 1);
    lcd.print("Reconectando...");
    
    conectarWifi();
  }
}

// Función para comprobar si debe desactivarse la bomba (después de dispensar el volumen objetivo)
void verificarBomba() {
  if (bombaTrabajando) {
    unsigned long tiempoActual = millis();
    unsigned long tiempoTranscurrido = tiempoActual - tiempoInicioBomba;
    
    // Calcular duración necesaria en ms para el volumen deseado
    int duracionNecesaria = (mlObjetivo * 1000) / CAUDAL_BOMBA;
    
    // Si ya se dispensó el volumen objetivo, desactivar la bomba
    if (tiempoTranscurrido >= duracionNecesaria) {
      Serial.println("Tiempo de riego completado. Desactivando bomba...");
      desactivarBomba();
      
      // Forzar una nueva lectura del sensor después del riego (pero esperando un poco)
      delay(1000);
      leerSensor();
    }
  }
}

void loop() {
  // Alimentar el watchdog en cada iteración del loop
  ESP.wdtFeed();
  
  // Verificar la conexión WiFi cada cierto tiempo
  verificarWifi();
  
  // Verificar si la bomba debe seguir encendida
  verificarBomba();
  
  // Leer mensajes de Telegram
  unsigned long currentMillis = millis();
  if (currentMillis - lastTimeBotRan > BOT_MTBS) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    
    while (numNewMessages) {
      Serial.println("Mensajes nuevos recibidos");
      handleNewMessages(numNewMessages);
      numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    }
    
    lastTimeBotRan = currentMillis;
  }
  
  // Leer sensor de humedad periódicamente
  if (currentMillis - ultimaLecturaSensor > INTERVALO_LECTURA) {
    leerSensor();
    ultimaLecturaSensor = currentMillis;
    
    // Verificar si se necesita riego automático (solo si está en modo viaje)
    verificarRiegoAutomatico();
  }
  
  // Actualizar LCD periódicamente (más frecuente que la lectura del sensor)
  if (currentMillis - ultimaActualizacionLCD > INTERVALO_LCD) {
    actualizarLCD();
    ultimaActualizacionLCD = currentMillis;
  }
  
  // Guardar estado en EEPROM periódicamente
  if (currentMillis - ultimoGuardadoEEPROM > INTERVALO_GUARDADO) {
    guardarEstado();
    ultimoGuardadoEEPROM = currentMillis;
  }
  
  // Pequeña pausa para evitar sobrecarga de la CPU
  delay(10);
}