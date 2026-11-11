// ================================================================
//  config.example.h -> MODELO do config.h (este vai para o GitHub).
//  Copie para config.h e preencha. O config.h fica no .gitignore.
//  Nunca imprima estes valores no Serial.
//
//  ATENCAO no Wokwi: quem tiver o link do projeto ve esta aba.
//  Nao compartilhe o link do Wokwi com chaves de verdade aqui dentro.
// ================================================================

// --- Wi-Fi. No Wokwi: "Wokwi-GUEST" e senha vazia ---
#define WIFI_SSID  "Wokwi-GUEST"
#define WIFI_PASS  ""

// --- Identidade (vai nos topicos: sis1a/<NOME>/<DISP>/...) ---
#define NOME       "pedrolovatto"
#define DISP       "geladeira01"

// --- Liga/desliga a nuvem (0 = so a logica local, o resto aparece no Serial) ---
#define USAR_MQTT    1
#define USAR_WORKER  1

// --- Broker MQTT com TLS (porta 8883 do test.mosquitto.org).
//     O TLS e' conferido com a CA_MOSQUITTO. Sem login: o login da porta
//     8885 e' publico (nao protegia nada) e o broker passou a recusa-lo.
//     Num produto real, cada dispositivo teria o seu usuario.
//     Para usar uma porta com login, preencha MQTT_USER e MQTT_PASS. ---
#define MQTT_HOST  "test.mosquitto.org"
#define MQTT_PORTA 8883
#define MQTT_USER  ""
#define MQTT_PASS  ""

// --- Worker da Cloudflare (sem barra no final) ---
#define WORKER_URL "https://frioseguro.pedrollovatto.workers.dev"

// --- Camada 2: API key (mesmo valor do Secret API_KEY no Worker) ---
#define API_KEY    "cole-aqui-a-sua-api-key"

// --- Camada 3: chave HMAC (mesmo valor do Secret CHAVE_HMAC no Worker
//     e do campo "Chave HMAC" no painel) ---
#define CHAVE_HMAC "cole-aqui-a-sua-chave-hmac"
