// ================================================================
//  FRIOSEGURO - monitor da geladeira de vacinas (Trabalho N2 de IoT)
//
//  O programa e' dividido em duas tarefas (FreeRTOS):
//    loop (prioridade 2) .... sensores, regras de alarme, buzzer e botao.
//                             Tem prioridade maior: sempre que precisa, ela
//                             passa na frente da rede, entao o alarme reage
//                             em < 1 s mesmo com a internet lenta ou fora (RNF1).
//    tarefaRede (prio. 1) ... Wi-Fi, relogio (NTP), MQTT com TLS, Worker
//                             (HTTPS) e a fila de leituras do RF4. Usa o
//                             tempo que sobra; se travar esperando a rede,
//                             o alarme nao percebe.
//  As duas conversam por filas do FreeRTOS: o loop entrega leituras e
//  alertas; a rede devolve o pedido de "silenciar" (ja validado).
//
//  Abas do projeto no Wokwi:
//    sketch.ino      -> este arquivo
//    config.h        -> segredos e chaves USAR_MQTT / USAR_WORKER
//    certificados.h  -> CAs publicas do broker e do Worker
//    diagram.json    -> o circuito
//    libraries.txt   -> PubSubClient, DHT sensor library, Adafruit Unified Sensor
// ================================================================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <time.h>
#include "mbedtls/md.h"   // HMAC-SHA256 (ja vem no core do ESP32)
#include "config.h"
#include "certificados.h"

// ---------------- Pinos (Etapa 2) ----------------
#define PIN_DHT     4
#define PIN_TRIG    18
#define PIN_ECHO    19
#define PIN_BUZZER  23
#define PIN_BOTAO   27

// ---------------- Regras (Etapas 1 e 3) ----------------
const uint32_t PERIODO_PORTA_MS    = 200;     // le a porta 5x por segundo
const uint32_t PERIODO_TEMP_MS     = 2000;    // o DHT22 nao aceita mais rapido
const uint32_t PERIODO_REGISTRO_MS = 20000;   // RF1: uma leitura a cada 20 s

const float    PORTA_ABERTA_CM     = 10.0;    // RF2: aberta acima de 10 cm
const float    PORTA_FECHADA_CM    = 5.0;     //      fechada abaixo de 5 cm
const uint32_t TOLERANCIA_PORTA_MS = 30000;   // RF2: alarme so depois de 30 s

const float    T_MAX = 8.0, T_MIN = 2.0;              // RF3: faixa da vacina
const float    T_VOLTA_MAX = 7.0, T_VOLTA_MIN = 3.0;  // histerese para desligar
const uint32_t TOLERANCIA_TEMP_MS  = 60000;           // RF3: fora por mais de 1 min

const int      MAX_INVALIDAS_PORTA = 5;       // 5 x 200 ms = 1 s sem leitura
const int      MAX_INVALIDAS_TEMP  = 3;       // 3 x 2 s    = 6 s sem leitura

const uint32_t SILENCIO_MS  = 10UL * 60 * 1000;  // "silenciar" vale 10 min
const uint32_t COOLDOWN_MS  = 10UL * 60 * 1000;  // alerta remoto: 1 por tipo a cada 10 min
const uint32_t JANELA_CMD_S = 30;                // comando com mais de 30 s e' velho

const int      FILA_MAX     = 360;            // RF4: 360 leituras = 2 h
const int      LOTE_MAX     = 60;             // leituras por POST no Worker
const int      ALERTAS_MAX  = 20;             // alertas guardados sem internet

// ---------------- Nomes que vao nas mensagens ----------------
enum Porta  : uint8_t { P_DESCONHECIDA, P_FECHADA, P_ABERTA };
enum Alarme : uint8_t { A_NENHUM, A_FALHA, A_PORTA, A_TEMP };   // em ordem de gravidade
enum Tipo   : uint8_t { T_TEMP, T_PORTA, T_FALHA_TEMP, T_FALHA_PORTA, N_TIPOS };
enum Evento : uint8_t { EV_INICIO, EV_LEMBRETE, EV_NORMALIZADO };

const char* NOME_PORTA[]  = { "desconhecida", "fechada", "aberta" };
const char* NOME_ALARME[] = { "nenhum", "falha_sensor", "porta", "temperatura" };
const char* NOME_TIPO[]   = { "temperatura_fora", "porta_aberta", "falha_sensor_temp", "falha_sensor_porta" };
const char* NOME_EVENTO[] = { "inicio", "lembrete", "normalizado" };

// ---------------- O que passa de uma tarefa para a outra ----------------
struct Leitura {           // uma linha do historico (a cada 20 s)
  uint32_t ms;             // millis() da leitura: vira horario na hora de enviar
  float    temp, umid, dist;
  uint8_t  porta, alarme;
};
struct Alerta {
  uint32_t ms;
  uint8_t  tipo, evento, porta;
  float    temp;
  uint32_t duracao_s;
};
struct Estado {            // retrato do momento, para o topico "estado"
  float    temp, umid;
  uint8_t  porta, alarme;
  uint32_t silencioAteMs;  // 0 = nao silenciado
};

QueueHandle_t qLeituras, qAlertas;
portMUX_TYPE  muxEstado = portMUX_INITIALIZER_UNLOCKED;
Estado        estadoCompart;
volatile bool estadoMudou     = true;
volatile bool quedaSimulada   = false;  // botao: simula a queda do Wi-Fi
volatile bool pedidoSilenciar = false;  // rede -> loop (comando ja validado)
volatile int  filaQtdVisivel  = 0;      // so para o log do loop

// =================================================================
//  SERIAL ORGANIZADO (blocos com separador, alarmes em destaque)
// =================================================================
String horaLocal() {
  extern volatile int32_t ajusteRelogio;
  time_t t = time(nullptr) + ajusteRelogio;
  char b[16];
  if (t < 1700000000) {                       // relogio ainda nao acertado
    snprintf(b, sizeof(b), "t+%lus", (unsigned long)(millis() / 1000));
    return String(b);
  }
  t -= 3 * 3600;                              // horario de Brasilia
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(b, sizeof(b), "%H:%M:%S", &tm);
  return String(b);
}

// Abre um bloco:  ------------- 13:16:20  REGISTRO -------------
void titulo(const char* nome) {
  Serial.printf("\n-------------------- %s  %s --------------------\n", horaLocal().c_str(), nome);
}

// Evento comum, numa linha:   > 13:16:02  porta ABERTA (40.0 cm)
void aviso(const char* fmt, ...) {
  char txt[200];
  va_list a; va_start(a, fmt); vsnprintf(txt, sizeof(txt), fmt, a); va_end(a);
  Serial.printf("  > %s  %s\n", horaLocal().c_str(), txt);
}

// Evento importante (alarme liga/desliga, falha), dentro de uma caixa
void destaque(const char* fmt, ...) {
  char txt[200];
  va_list a; va_start(a, fmt); vsnprintf(txt, sizeof(txt), fmt, a); va_end(a);
  Serial.printf("\n************************************************************\n"
                "  %s  %s\n"
                "************************************************************\n",
                horaLocal().c_str(), txt);
}

// =================================================================
//  TAREFA LOOP - SENTIR, DECIDIR E AGIR (nao depende da rede)
// =================================================================
DHT dht(PIN_DHT, DHT22);

Porta    porta = P_DESCONHECIDA;
float    distCm = NAN;
int      invalidasPorta = 0;
bool     falhaPorta = false, alarmePorta = false;
uint32_t portaAbertaDesde = 0;

float    temp = NAN, umid = NAN;
int      invalidasTemp = 0;
bool     falhaTemp = false, alarmeTemp = false, foraDaFaixa = false;
uint32_t foraDesde = 0;

bool     silencioAtivo = false;
uint32_t silencioAte = 0;
uint8_t  mascaraSilenciada = 0;
int      freqAtual = -1;

bool     ativoAntes[N_TIPOS] = {}, jaAlertou[N_TIPOS] = {}, alertouEpisodio[N_TIPOS] = {};
bool     pendenteInicio[N_TIPOS] = {};
uint32_t ultimoAlertaMs[N_TIPOS] = {}, inicioMs[N_TIPOS] = {};

uint8_t alarmeAtual() {
  if (alarmeTemp)              return A_TEMP;
  if (alarmePorta)             return A_PORTA;
  if (falhaTemp || falhaPorta) return A_FALHA;
  return A_NENHUM;
}

// Copia o estado para a tarefa de rede. avisar=true -> publica no topico "estado"
void atualizarEstado(bool avisar) {
  Estado e = { falhaTemp ? NAN : temp, falhaTemp ? NAN : umid, (uint8_t)porta, alarmeAtual(),
               silencioAtivo ? silencioAte : 0 };
  portENTER_CRITICAL(&muxEstado);
  estadoCompart = e;
  if (avisar) estadoMudou = true;
  portEXIT_CRITICAL(&muxEstado);
}

// ---------- Porta (HC-SR04) ----------
float lerDistanciaCm() {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, 30000UL);   // espera no maximo 30 ms
  if (us == 0) return 0;                                  // sem eco: fio solto ou fora do alcance
  return us * 0.0343f / 2.0f;
}

void processarPorta(uint32_t agora) {
  float d = lerDistanciaCm();
  distCm = d;

  // Leitura invalida: 0 (sem eco) ou fora de 2-400 cm. Nunca vira "fechada".
  if (d < 2.0f || d > 400.0f) {
    if (++invalidasPorta >= MAX_INVALIDAS_PORTA && !falhaPorta) {
      falhaPorta = true;
      porta = P_DESCONHECIDA;
      alarmePorta = false;            // vira alarme de FALHA (bipe curto)
      destaque("FALHA no sensor da porta: 1 s sem leitura valida -> porta DESCONHECIDA");
      atualizarEstado(true);
    }
    return;
  }
  invalidasPorta = 0;
  if (falhaPorta) {
    falhaPorta = false;
    aviso("sensor da porta voltou a responder");
    atualizarEstado(true);
  }

  Porta antes = porta;
  if (d > PORTA_ABERTA_CM)       porta = P_ABERTA;
  else if (d < PORTA_FECHADA_CM) porta = P_FECHADA;
  else if (porta == P_DESCONHECIDA) porta = P_FECHADA;   // 5-10 cm logo ao ligar
  // entre 5 e 10 cm: mantem o estado anterior (histerese)

  if (porta != antes) {
    if (porta == P_ABERTA) portaAbertaDesde = agora;
    aviso("porta %s (%.1f cm)", NOME_PORTA[porta], d);
    atualizarEstado(true);
  }

  bool deve = alarmePorta;
  if (porta == P_ABERTA && agora - portaAbertaDesde > TOLERANCIA_PORTA_MS) deve = true;
  if (porta == P_FECHADA) deve = false;
  if (deve != alarmePorta) {
    alarmePorta = deve;
    if (deve) destaque("ALARME DA PORTA LIGADO: aberta ha mais de 30 s");
    else      destaque("alarme da porta desligado: a porta fechou");
    atualizarEstado(true);
  }
}

// ---------- Temperatura (DHT22) ----------
void processarTemperatura(uint32_t agora) {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t)) {
    // Uma falha isolada nao muda nada; 3 seguidas viram FALHA.
    // Leitura invalida nunca conta como "temperatura ok".
    if (++invalidasTemp >= MAX_INVALIDAS_TEMP && !falhaTemp) {
      falhaTemp = true;
      destaque("FALHA no sensor de temperatura: 3 leituras invalidas seguidas");
      atualizarEstado(true);
    }
    return;
  }
  invalidasTemp = 0;
  if (falhaTemp) {
    falhaTemp = false;
    aviso("sensor de temperatura voltou a responder");
    atualizarEstado(true);
  }
  bool primeira = isnan(temp);   // primeira leitura valida desde que ligou
  temp = t;
  umid = h;
  if (primeira) atualizarEstado(true);   // o "estado" ja sai com a temperatura

  if (t > T_MAX || t < T_MIN) {
    if (!foraDaFaixa) {
      foraDaFaixa = true;
      foraDesde = agora;
      aviso("%.1f C fora de 2-8 C: cronometro de 1 min iniciado", t);
    } else if (!alarmeTemp && agora - foraDesde > TOLERANCIA_TEMP_MS) {
      alarmeTemp = true;
      destaque("ALARME DE TEMPERATURA LIGADO: %.1f C fora da faixa ha mais de 1 min", t);
      atualizarEstado(true);
    }
  } else {
    if (foraDaFaixa) {
      foraDaFaixa = false;            // zera o cronometro
      if (!alarmeTemp) aviso("%.1f C: voltou para a faixa antes de 1 min, nada a fazer", t);
    }
    if (alarmeTemp && t >= T_VOLTA_MIN && t <= T_VOLTA_MAX) {
      alarmeTemp = false;
      destaque("alarme de temperatura desligado: %.1f C, voltou para 3-7 C", t);
      atualizarEstado(true);
    }
    // entre 2-3 C e 7-8 C com o alarme ligado: mantem (histerese)
  }
}

// ---------- Alertas remotos com cooldown (Etapa 6) ----------
void emitirAlerta(int tipo, Evento ev, uint32_t agora) {
  Alerta a = { agora, (uint8_t)tipo, (uint8_t)ev, (uint8_t)porta, falhaTemp ? NAN : temp,
               (agora - inicioMs[tipo]) / 1000 };
  aviso("alerta remoto: %s (%s)", NOME_TIPO[tipo], NOME_EVENTO[ev]);
  if (xQueueSend(qAlertas, &a, 0) != pdTRUE) aviso("fila interna cheia: alerta perdido");
}

void gerenciarAlertas(uint32_t agora) {
  bool ativo[N_TIPOS] = { alarmeTemp, alarmePorta, falhaTemp, falhaPorta };
  for (int i = 0; i < N_TIPOS; i++) {
    if (ativo[i] && !ativoAntes[i]) {          // um alarme comecou
      inicioMs[i] = agora;
      pendenteInicio[i] = true;
      alertouEpisodio[i] = false;
    }
    if (ativo[i]) {
      // Mesmo tipo de alerta no maximo a cada 10 min. Enquanto o alarme
      // continuar, isso vira um lembrete a cada 10 min.
      bool liberado = !jaAlertou[i] || (agora - ultimoAlertaMs[i] >= COOLDOWN_MS);
      if (liberado) {
        emitirAlerta(i, pendenteInicio[i] ? EV_INICIO : EV_LEMBRETE, agora);
        pendenteInicio[i] = false;
        jaAlertou[i] = true;
        alertouEpisodio[i] = true;
        ultimoAlertaMs[i] = agora;
      }
    } else if (ativoAntes[i] && alertouEpisodio[i]) {
      emitirAlerta(i, EV_NORMALIZADO, agora);   // o alarme terminou
      alertouEpisodio[i] = false;
      pendenteInicio[i] = false;
    }
    ativoAntes[i] = ativo[i];
  }
}

// ---------- Buzzer: um padrao por alarme ----------
#define BIT_TEMP  0x01
#define BIT_PORTA 0x02
#define BIT_FALHA 0x04

uint8_t mascaraAtiva() {
  return (alarmeTemp ? BIT_TEMP : 0) | (alarmePorta ? BIT_PORTA : 0) |
         ((falhaTemp || falhaPorta) ? BIT_FALHA : 0);
}

void atualizarBuzzer(uint32_t agora) {
  if (pedidoSilenciar) {                         // veio do painel, ja validado
    pedidoSilenciar = false;
    silencioAtivo = true;
    silencioAte = agora + SILENCIO_MS;
    mascaraSilenciada = mascaraAtiva();          // so os alarmes de agora ficam mudos
    destaque("buzzer SILENCIADO por 10 min (o alarme continua ativo)");
    atualizarEstado(true);
  }
  if (silencioAtivo && (int32_t)(agora - silencioAte) >= 0) {
    silencioAtivo = false;
    mascaraSilenciada = 0;
    aviso("fim dos 10 min de silencio do buzzer");
    atualizarEstado(true);
  }

  // Alarme de um tipo novo toca mesmo durante o silencio
  uint8_t soando = mascaraAtiva() & (silencioAtivo ? ~mascaraSilenciada : 0xFF);
  int freq = 0;
  if (soando & BIT_TEMP)       freq = 2000;                                   // continuo
  else if (soando & BIT_PORTA) freq = (agora % 1000 < 500) ? 1500 : 0;        // intermitente
  else if (soando & BIT_FALHA) freq = (agora % 5000 < 150) ? 1000 : 0;        // bipe a cada 5 s

  if (freq != freqAtual) {
    if (freq) tone(PIN_BUZZER, freq); else noTone(PIN_BUZZER);
    freqAtual = freq;
  }
}

// ---------- Botao: simula a queda do Wi-Fi (teste do RF4) ----------
void lerBotao(uint32_t agora) {
  static bool ultimo = HIGH;
  static uint32_t ultimaMudanca = 0;
  bool b = digitalRead(PIN_BOTAO);
  if (b != ultimo && agora - ultimaMudanca > 50) {     // debounce de 50 ms
    ultimaMudanca = agora;
    ultimo = b;
    if (b == LOW) {
      quedaSimulada = !quedaSimulada;
      if (quedaSimulada) destaque("BOTAO: queda de Wi-Fi SIMULADA (aperte de novo para voltar)");
      else               destaque("BOTAO: Wi-Fi liberado de novo");
    }
  }
}

// ---------- Registro a cada 20 s (RF1) ----------
void registrar(uint32_t agora) {
  Leitura l = { agora, falhaTemp ? NAN : temp, falhaTemp ? NAN : umid, distCm, (uint8_t)porta, alarmeAtual() };
  titulo("REGISTRO (a cada 20 s)");
  Serial.printf("  temperatura : %.1f C      umidade: %.0f %%\n", l.temp, l.umid);
  Serial.printf("  porta       : %s (%.1f cm)\n", NOME_PORTA[porta], distCm);
  Serial.printf("  alarme      : %s%s\n", NOME_ALARME[l.alarme], silencioAtivo ? " (buzzer silenciado)" : "");
  Serial.printf("  fila        : %d leitura(s) esperando envio\n", filaQtdVisivel);
  if (xQueueSend(qLeituras, &l, 0) != pdTRUE) aviso("fila interna cheia: leitura perdida");
}

// =================================================================
//  TAREFA DE REDE - COMUNICAR E GUARDAR (Wi-Fi, NTP, MQTT, Worker, fila)
// =================================================================
WiFiClientSecure tlsMqtt;
PubSubClient     mqtt(tlsMqtt);
String T_TELE, T_ESTADO, T_ALERTA, T_CMD, T_ACK;

Leitura  fila[FILA_MAX];        // RF4: buffer circular das leituras ainda nao gravadas
int      filaIni = 0, filaQtd = 0;
Alerta   filaAl[ALERTAS_MAX];
int      alIni = 0, alQtd = 0;
uint32_t ultimoTsCmd = 0;       // anti-replay dos comandos

bool     relogioOk()  { return time(nullptr) > 1700000000; }
// Diferenca entre o relogio do Worker e o do ESP32. Cada resposta do Worker
// traz a hora dele ("agora"). No Wokwi a simulacao as vezes anda mais devagar
// que o tempo real e o relogio do ESP32 fica para tras; com este ajuste o
// horario das leituras e do envio continua certo (num ESP32 real fica ~0).
volatile int32_t ajusteRelogio = 0;
uint32_t agoraEpoch() { return (uint32_t)time(nullptr) + ajusteRelogio; }
bool     ultimoFoiAtrasado = false;   // o Worker recusou por horario: reenviar ja
uint32_t tsDe(uint32_t ms) { return agoraEpoch() - (millis() - ms) / 1000; }  // horario original
uint32_t wifiOkDesde = 0;      // quando o Wi-Fi conectou (millis)
// So considera "online" 1,5 s depois de conectar: abrir uma conexao TLS no
// mesmo instante em que o Wi-Fi acabou de subir derrubava o ESP32 no Wokwi.
bool     online()     { return !quedaSimulada && WiFi.status() == WL_CONNECTED && millis() - wifiOkDesde > 1500; }
String   num(float v, int casas) { return isnan(v) ? String("null") : String(v, casas); }

bool mqttPronto() {
#if USAR_MQTT
  return online() && mqtt.connected();
#else
  return online();
#endif
}

// HMAC-SHA256(CHAVE_HMAC, msg) em hexadecimal (64 caracteres)
String hmacHex(const String& msg) {
  uint8_t mac[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                  (const uint8_t*)CHAVE_HMAC, strlen(CHAVE_HMAC),
                  (const uint8_t*)msg.c_str(), msg.length(), mac);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", mac[i]);
  hex[64] = '\0';
  return String(hex);
}

// Compara sem revelar, pelo tempo, onde esta a diferenca
bool iguais(const String& a, const String& b) {
  if (a.length() != b.length()) return false;
  uint8_t dif = 0;
  for (unsigned int i = 0; i < a.length(); i++) dif |= a[i] ^ b[i];
  return dif == 0;
}

bool publicar(const String& topico, const String& msg, bool retain = false) {
#if USAR_MQTT
  bool ok = mqtt.publish(topico.c_str(), msg.c_str(), retain);
  Serial.printf("  MQTT %s %s\n      %s\n", ok ? "->" : "FALHOU", topico.substring(topico.lastIndexOf('/') + 1).c_str(), msg.c_str());
  return ok;
#else
  Serial.printf("  MQTT (desligado, so no Serial) -> %s\n      %s\n", topico.substring(topico.lastIndexOf('/') + 1).c_str(), msg.c_str());
  return true;
#endif
}

// ---------- Comandos assinados: comando|ts|assinatura ----------
void responder(const String& texto) { publicar(T_ACK, texto); }

void aoReceber(char* topico, byte* payload, unsigned int len) {
  String msg;
  msg.reserve(len);
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];   // copia antes de publicar
  titulo("COMANDO RECEBIDO");
  Serial.printf("      %s\n", msg.c_str());

  int p1 = msg.indexOf('|'), p2 = msg.lastIndexOf('|');
  if (p1 < 0 || p2 <= p1) { responder("RECUSADO: sem assinatura"); return; }
  String cmd = msg.substring(0, p1), tsTxt = msg.substring(p1 + 1, p2), sig = msg.substring(p2 + 1);
  sig.toLowerCase();

  if (!iguais(sig, hmacHex(cmd + "|" + tsTxt))) { responder("RECUSADO: assinatura invalida|" + tsTxt); return; }
  uint32_t ts = strtoul(tsTxt.c_str(), NULL, 10), t = agoraEpoch();
  if (ts + JANELA_CMD_S < t || ts > t + JANELA_CMD_S) { responder("RECUSADO: comando expirado|" + tsTxt); return; }
  if (ts <= ultimoTsCmd) { responder("RECUSADO: replay|" + tsTxt); return; }
  ultimoTsCmd = ts;

  if (cmd == "silenciar") {
    Estado e;
    portENTER_CRITICAL(&muxEstado); e = estadoCompart; portEXIT_CRITICAL(&muxEstado);
    if (e.alarme == A_NENHUM) { responder("RECUSADO: nenhum alarme ativo|" + tsTxt); return; }
    pedidoSilenciar = true;                       // o loop aplica em poucos ms
    responder("OK: silenciar|" + tsTxt);
  } else {
    responder("RECUSADO: comando desconhecido|" + tsTxt);
  }
}

// ---------- Wi-Fi: reconecta a cada 20 s sem travar nada ----------
void cuidarWifi() {
  static bool conectadoAntes = false, simulandoAntes = false;
  static uint32_t ultimaTentativa = 0;

  if (quedaSimulada) {
    if (!simulandoAntes) {
      simulandoAntes = true;
      WiFi.disconnect();   // sem despedida do MQTT: o broker publica o LWT sozinho
      aviso("Wi-Fi desligado (queda simulada). As leituras vao para a fila.");
    }
    conectadoAntes = false;
    return;
  }
  if (simulandoAntes) { simulandoAntes = false; ultimaTentativa = 0; }

  bool c = WiFi.status() == WL_CONNECTED;
  if (c && !conectadoAntes) { wifiOkDesde = millis(); aviso("Wi-Fi conectado | IP %s", WiFi.localIP().toString().c_str()); }
  if (!c && conectadoAntes) aviso("Wi-Fi caiu. As leituras vao para a fila.");
  conectadoAntes = c;

  // Espera 20 s entre tentativas: interromper uma conexao que ainda esta
  // em andamento (o Wokwi as vezes demora mais de 10 s) faz ela nunca terminar.
  if (!c && (ultimaTentativa == 0 || millis() - ultimaTentativa >= 20000)) {
    ultimaTentativa = millis();
    int st = WiFi.status();   // 1 = rede nao encontrada, 4 = falhou, 6 = desconectado
    if (st != WL_IDLE_STATUS) { WiFi.disconnect(); vTaskDelay(pdMS_TO_TICKS(200)); }
    // A rede do Wokwi fica no canal 6: informar o canal pula a busca e conecta mais rapido
    int canal = (strcmp(WIFI_SSID, "Wokwi-GUEST") == 0) ? 6 : 0;
    WiFi.begin(WIFI_SSID, WIFI_PASS, canal);
    aviso("Wi-Fi: tentando conectar em %s... (status anterior: %d)", WIFI_SSID, st);
  }
}

void cuidarRelogio() {
  static bool avisou = false;
  if (!avisou && relogioOk()) {
    avisou = true;
    aviso("relogio acertado pelo NTP (epoch %lu)", (unsigned long)agoraEpoch());
  }
}

// ---------- MQTT com TLS + login + LWT ----------
void cuidarMqtt() {
#if USAR_MQTT
  if (mqtt.connected()) { mqtt.loop(); return; }
  static uint32_t ultima = 0;
  if (!relogioOk()) return;                                // TLS confere a validade pelo relogio
  if (ultima != 0 && millis() - ultima < 10000) return;    // tenta de novo a cada 10 s
  ultima = millis();

  String id = String("esp32-") + NOME + "-" + String(esp_random() % 10000);
  aviso("MQTT: conectando em %s:%d (TLS%s)...", MQTT_HOST, MQTT_PORTA, strlen(MQTT_USER) ? " + login" : "");
  // Usuario vazio no config.h = porta sem login (ex.: 8883)
  const char* usuario = strlen(MQTT_USER) ? MQTT_USER : nullptr;
  const char* senha   = strlen(MQTT_USER) ? MQTT_PASS : nullptr;
  // LWT: se o ESP32 sumir sem avisar, o broker publica {"online":false} no "estado"
  if (mqtt.connect(id.c_str(), usuario, senha, T_ESTADO.c_str(), 1, true, "{\"online\":false}")) {
    aviso("MQTT conectado");
    mqtt.subscribe(T_CMD.c_str());
    estadoMudou = true;                                    // avisa que voltou
  } else {
    int rc = mqtt.state();
    if (rc < 0) {                                          // falhou antes do broker responder (rede/TLS)
      char erro[120];
      tlsMqtt.lastError(erro, sizeof(erro));
      aviso("MQTT falhou rc=%d | TLS: %s (tento de novo em 10 s)", rc, erro);
    } else {                                               // o broker respondeu e recusou
      aviso("MQTT recusado pelo broker rc=%d (%s) (tento de novo em 10 s)", rc,
            rc == 4 ? "usuario/senha errados" : rc == 5 ? "nao autorizado" : "outro motivo");
    }
  }
#endif
}

// ---------- Telemetria (tempo real) ----------
void publicarTelemetria(const Leitura& l) {
  if (!mqttPronto() || !relogioOk()) return;   // sem rede, a fila garante o historico
  String msg = String("{\"disp\":\"") + DISP + "\",\"ts\":" + String(tsDe(l.ms)) +
               ",\"temp_c\":" + num(l.temp, 1) + ",\"umid_pct\":" + num(l.umid, 1) +
               ",\"dist_cm\":" + num(l.dist, 1) + ",\"porta\":\"" + NOME_PORTA[l.porta] +
               "\",\"alarme\":\"" + NOME_ALARME[l.alarme] + "\"}";
  publicar(T_TELE, msg);
}

void publicarEstadoSeMudou() {
  if (!estadoMudou || !mqttPronto() || !relogioOk()) return;
  Estado e;
  portENTER_CRITICAL(&muxEstado);
  e = estadoCompart;
  estadoMudou = false;
  portEXIT_CRITICAL(&muxEstado);

  uint32_t silAte = 0;
  if (e.silencioAteMs) silAte = agoraEpoch() + (uint32_t)((int32_t)(e.silencioAteMs - millis()) / 1000);
  String msg = String("{\"online\":true,\"ts\":") + String(agoraEpoch()) +
               ",\"temp_c\":" + num(e.temp, 1) + ",\"umid_pct\":" + num(e.umid, 1) +
               ",\"porta\":\"" + NOME_PORTA[e.porta] + "\",\"alarme\":\"" + NOME_ALARME[e.alarme] +
               "\",\"silenciado_ate\":" + String(silAte) + "}";
  publicar(T_ESTADO, msg, true);               // retain: quem abrir o painel ja recebe
}

// ---------- Alertas (guardados se estiver sem internet) ----------
void guardarAlerta(const Alerta& a) {
  if (alQtd == ALERTAS_MAX) { alIni = (alIni + 1) % ALERTAS_MAX; alQtd--; }
  filaAl[(alIni + alQtd) % ALERTAS_MAX] = a;
  alQtd++;
}

void publicarAlertasPendentes() {
  while (alQtd > 0 && mqttPronto() && relogioOk()) {
    const Alerta& a = filaAl[alIni];
    String msg = String("{\"disp\":\"") + DISP + "\",\"ts\":" + String(tsDe(a.ms)) +
                 ",\"tipo\":\"" + NOME_TIPO[a.tipo] + "\",\"evento\":\"" + NOME_EVENTO[a.evento] +
                 "\",\"temp_c\":" + num(a.temp, 1) + ",\"porta\":\"" + NOME_PORTA[a.porta] +
                 "\",\"duracao_s\":" + String(a.duracao_s) + "}";
    if (!publicar(T_ALERTA, msg)) break;
    alIni = (alIni + 1) % ALERTAS_MAX;
    alQtd--;
  }
}

// ---------- Fila do RF4 e envio ao Worker ----------
void enfileirar(const Leitura& l) {
  if (filaQtd == FILA_MAX) {                   // cheia: sobrescreve a mais antiga
    filaIni = (filaIni + 1) % FILA_MAX;
    filaQtd--;
    aviso("fila cheia: a leitura mais antiga foi sobrescrita");
  }
  fila[(filaIni + filaQtd) % FILA_MAX] = l;
  filaQtd++;
  filaQtdVisivel = filaQtd;
  if (!online()) aviso("sem internet: %d leitura(s) guardada(s) na fila", filaQtd);
}

#if USAR_WORKER
// A conexao com o Worker fica ABERTA entre um envio e outro (keep-alive).
// Negociar o TLS e' lento (no Wokwi leva varios segundos); assim isso so
// acontece na primeira vez ou depois que a conexao cair.
WiFiClientSecure tlsWorker;
HTTPClient       httpWorker;
bool             workerPreparado = false;

// Abre a conexao HTTPS com o Worker (ou reaproveita a que ja esta aberta).
// E' chamada ANTES de montar o envelope: assim o horario de envio ("ts")
// e' marcado depois da negociacao lenta do TLS, e nao antes.
bool conectarWorker() {
  if (!workerPreparado) {
    tlsWorker.setCACert(CA_WORKER);            // Camada 1: confere o certificado do Worker
    httpWorker.setReuse(true);
    httpWorker.setTimeout(15000);
    workerPreparado = true;
  }
  if (tlsWorker.connected()) return true;
  String host = String(WORKER_URL);
  host.replace("https://", "");
  int barra = host.indexOf('/');
  if (barra >= 0) host = host.substring(0, barra);
  uint32_t inicio = millis();
  bool ok = tlsWorker.connect(host.c_str(), 443);
  if (ok) {
    aviso("Worker: nova conexao HTTPS aberta (%.1f s)", (millis() - inicio) / 1000.0);
  } else {
    char erro[120];
    tlsWorker.lastError(erro, sizeof(erro));
    aviso("Worker: nao consegui abrir a conexao HTTPS | %s", erro);
  }
  return ok;
}
#endif

// Devolve o codigo HTTP (200 = gravou)
int enviarAoWorker(const String& corpo, int n) {
#if USAR_WORKER
  uint32_t inicio = millis();
  if (!httpWorker.begin(tlsWorker, String(WORKER_URL) + "/insert")) return -1;
  httpWorker.addHeader("Content-Type", "application/json");
  httpWorker.addHeader("X-API-Key", API_KEY);              // Camada 2
  httpWorker.addHeader("X-Assinatura", hmacHex(corpo));    // Camada 3: assina o corpo exato
  int codigo = httpWorker.POST(corpo);
  String resposta = httpWorker.getString();
  httpWorker.end();
  Serial.printf("  WORKER -> %d leitura(s) | HTTP %d | %.1f s\n      %s\n",
                n, codigo, (millis() - inicio) / 1000.0, resposta.c_str());
  // Acerta o relogio pela hora do Worker
  int p = resposta.indexOf("\"agora\":");
  if (p >= 0) {
    uint32_t horaWorker = strtoul(resposta.c_str() + p + 8, nullptr, 10);
    if (horaWorker > 1700000000) {
      int32_t novo = (int32_t)(horaWorker - (uint32_t)time(nullptr));
      if (abs(novo - ajusteRelogio) > 5) aviso("relogio ajustado pela hora do Worker (%+ld s)", (long)novo);
      ajusteRelogio = novo;
    }
  }
  ultimoFoiAtrasado = (codigo == 401 && resposta.indexOf("atrasado") >= 0);
  if (codigo < 0) {
    char erro[120];
    tlsWorker.lastError(erro, sizeof(erro));
    if (codigo == HTTPC_ERROR_CONNECTION_LOST || codigo == HTTPC_ERROR_SEND_PAYLOAD_FAILED ||
        codigo == HTTPC_ERROR_SEND_HEADER_FAILED || codigo == HTTPC_ERROR_NOT_CONNECTED)
      Serial.println("      (a Cloudflare fechou a conexao parada; abro outra e reenvio)");
    else
      Serial.printf("      erro TLS: %s\n", erro);
    tlsWorker.stop();                          // descarta a conexao com problema; a proxima abre outra
  }
  return codigo;
#else
  Serial.printf("  WORKER (desligado, so no Serial) -> gravaria %d leitura(s)\n      %s\n", n, corpo.c_str());
  return 200;
#endif
}

void descarregarFila() {
  static uint32_t proximo = 0;
  if (filaQtd == 0 || !online() || !relogioOk() || (int32_t)(millis() - proximo) < 0) return;

#if USAR_WORKER
  if (!conectarWorker()) { proximo = millis() + 10000; return; }   // conecta ANTES de marcar o "ts"
#endif
  int n = filaQtd < LOTE_MAX ? filaQtd : LOTE_MAX;
  // Envelope assinado: "ts" = horario do ENVIO (anti-replay do Worker);
  // cada leitura leva o seu "ts_leitura" (horario original).
  String corpo = String("{\"disp\":\"") + DISP + "\",\"ts\":" + String(agoraEpoch()) + ",\"leituras\":[";
  for (int i = 0; i < n; i++) {
    const Leitura& l = fila[(filaIni + i) % FILA_MAX];
    if (i) corpo += ",";
    corpo += String("{\"ts_leitura\":") + String(tsDe(l.ms)) + ",\"temp_c\":" + num(l.temp, 1) +
             ",\"umid_pct\":" + num(l.umid, 1) + ",\"porta\":\"" + NOME_PORTA[l.porta] +
             "\",\"alarme\":\"" + NOME_ALARME[l.alarme] + "\"}";
  }
  corpo += "]}";

  int codigo = enviarAoWorker(corpo, n);
  if (codigo == 200 || codigo == 400) {        // 400 = dado recusado: reenviar nao adianta
    if (codigo == 400) aviso("o Worker recusou o lote (HTTP 400): descartado");
    filaIni = (filaIni + n) % FILA_MAX;
    filaQtd -= n;
    filaQtdVisivel = filaQtd;
    proximo = millis() + 1000;                 // proximo lote em 1 s
  } else if (ultimoFoiAtrasado) {
    proximo = millis() + 500;                  // relogio ja foi corrigido: reenvia em seguida
  } else if (codigo == HTTPC_ERROR_CONNECTION_LOST || codigo == HTTPC_ERROR_SEND_PAYLOAD_FAILED ||
             codigo == HTTPC_ERROR_SEND_HEADER_FAILED || codigo == HTTPC_ERROR_NOT_CONNECTED) {
    proximo = millis();                        // conexao velha caiu: reenvia ja, por uma conexao nova
  } else {
    proximo = millis() + 10000;                // outro erro: tenta de novo em 10 s
  }
}

// ---------- A tarefa de rede ----------
void tarefaRede(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);                         // quem reconecta e' o cuidarWifi()
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");   // UTC
#if USAR_MQTT
  tlsMqtt.setCACert(CA_MOSQUITTO);                     // Camada 1 no MQTT (nada de setInsecure)
  mqtt.setServer(MQTT_HOST, MQTT_PORTA);
  mqtt.setBufferSize(512);
  // A cada 30 s sem trafego o ESP32 manda um "estou vivo" ao broker. Se ele
  // sumir, o broker publica o LWT ~45 s depois. (15 s era pouco no Wokwi: com a
  // simulacao lenta, o broker achava que o ESP32 tinha caido sem ter caido.)
  mqtt.setKeepAlive(30);
  mqtt.setCallback(aoReceber);
#endif

  for (;;) {
    cuidarWifi();
    cuidarRelogio();

    Leitura l;
    while (xQueueReceive(qLeituras, &l, 0) == pdTRUE) {
      publicarTelemetria(l);   // tempo real (so se estiver conectado)
      enfileirar(l);           // historico: sai da fila quando o Worker confirmar
    }
    Alerta a;
    while (xQueueReceive(qAlertas, &a, 0) == pdTRUE) guardarAlerta(a);

    if (online()) {
      cuidarMqtt();
      publicarEstadoSeMudou();
      publicarAlertasPendentes();
      descarregarFila();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// =================================================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BOTAO, INPUT_PULLUP);
  dht.begin();

  String base = String("sis1a/") + NOME + "/" + DISP + "/";
  T_TELE = base + "telemetria";  T_ESTADO = base + "estado";  T_ALERTA = base + "alerta";
  T_CMD  = base + "comando";     T_ACK    = base + "ack";

  qLeituras = xQueueCreate(8, sizeof(Leitura));
  qAlertas  = xQueueCreate(8, sizeof(Alerta));
  atualizarEstado(true);

  Serial.println();
  Serial.println("============================================================");
  Serial.println("  FrioSeguro - monitor da geladeira de vacinas");
  Serial.println("============================================================");
  Serial.printf("MQTT: %s | Worker: %s\n", USAR_MQTT ? "ligado" : "desligado (so no Serial)",
                USAR_WORKER ? "ligado" : "desligado (so no Serial)");
  Serial.println("Clique no DHT22 ou no HC-SR04 durante a simulacao para mudar os valores.");
  Serial.println("Botao azul: simula a queda do Wi-Fi (teste do RF4).");
  Serial.println("============================================================");

  // As duas tarefas ficam no nucleo 1; o nucleo 0 fica livre para o Wi-Fi do
  // proprio ESP32. O loop ganha prioridade 2 e a rede fica com 1: o alarme
  // sempre passa na frente. Isso tambem evita o "watchdog": as contas pesadas
  // do TLS (varios segundos no Wokwi) nao ocupam mais o nucleo 0, que e' o
  // nucleo vigiado.
  vTaskPrioritySet(NULL, 2);
  xTaskCreatePinnedToCore(tarefaRede, "rede", 24576, NULL, 1, NULL, 1);
}

void loop() {
  static uint32_t tPorta = 0, tTemp = 0, tReg = 0;
  uint32_t agora = millis();

  lerBotao(agora);
  if (agora - tPorta >= PERIODO_PORTA_MS) { tPorta = agora; processarPorta(agora); }
  if (agora - tTemp  >= PERIODO_TEMP_MS)  { tTemp  = agora; processarTemperatura(agora); }
  gerenciarAlertas(agora);
  atualizarBuzzer(agora);
  if (agora - tReg >= PERIODO_REGISTRO_MS) {
    tReg = agora;
    atualizarEstado(false);   // mantem a temperatura do "estado" atualizada
    registrar(agora);
  }
  delay(5);
}
