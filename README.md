# FrioSeguro: monitor de geladeira de vacinas

Trabalho N2 de IoT (AMF, 2026-02), desafio **Geladeira de vacinas**.

Um ESP32 vigia a geladeira de vacinas do posto de saúde. Ele mede a temperatura (DHT22) e vê se a porta está aberta (HC-SR04). Se a temperatura sair de 2 a 8 °C por mais de 1 min, ou se a porta ficar aberta por mais de 30 s, ele toca o buzzer e avisa o painel, mesmo sem internet. Cada leitura vai para um histórico na Cloudflare (Worker + D1), que prova que as vacinas ficaram na faixa. Sem internet, as leituras esperam numa fila de até 2 h e são enviadas quando a conexão volta.

O caderno completo está em [`docs/Entrega.docx`](docs/Entrega.docx).

## Estrutura

| Pasta | O que tem |
|---|---|
| `sketch/` | Firmware do ESP32 para o Wokwi: `sketch.ino`, `certificados.h` (CAs públicas), `config.example.h` (modelo, sem chaves), `diagram.json` (circuito) e `libraries.txt` |
| `worker/` | Cloudflare Worker (`src/index.js`), tabelas do D1 (`schema.sql`) e o teste de ataques (`teste_ataques.js`) |
| `painel/` | Painel web em um arquivo só (`index.html`) |
| `docs/` | Caderno de projeto |

## Como montar

**1. Chaves.** Gere duas chaves aleatórias, a API key e a chave HMAC. Por exemplo, no console do navegador (F12):

```js
[...crypto.getRandomValues(new Uint8Array(32))].map(b => b.toString(16).padStart(2, "0")).join("")
```

**2. Cloudflare (pelo painel da Cloudflare):**
1. Crie um banco D1 e rode o `worker/schema.sql` no Console dele.
2. Crie um Worker, cole o `worker/src/index.js` e publique (Deploy).
3. Em *Settings > Bindings*, ligue o D1 com o nome `DB`.
4. Em *Settings > Variables and Secrets*, crie os Secrets `API_KEY` e `CHAVE_HMAC` com as chaves do passo 1.
5. Em *Settings > Triggers*, crie o Cron Trigger `0 6 * * *`. Ele roda a limpeza diária: leituras com mais de 12 meses e tentativas com mais de 30 dias.
6. Teste: abrir `https://<seu-worker>.workers.dev/` deve responder `FrioSeguro Worker: ok`.

**3. Wokwi:**
1. Crie um projeto ESP32 e coloque nele os arquivos da pasta `sketch/`.
2. Crie a aba `config.h` copiando o `config.example.h`. Preencha `API_KEY`, `CHAVE_HMAC` e, se mudou, `WORKER_URL`.
3. Rode a simulação. O Serial mostra a conexão com o Wi-Fi, com o MQTT (porta 8883, TLS) e com o Worker (HTTP 200).

Atenção: quem tiver o link do projeto no Wokwi vê a aba `config.h`. Não compartilhe o link com chaves de verdade nela.

**4. Painel:** abra o `painel/index.html` no navegador. Para silenciar o buzzer, cole a `CHAVE_HMAC` no campo do painel. Ela fica só na memória da aba.

## Como testar

| O quê | Como | Esperado |
|---|---|---|
| RF1, telemetria | Assinar `sis1a/pedrolovatto/geladeira01/#` (painel ou cliente MQTT) | uma mensagem em `telemetria` a cada 20 s |
| RF2, porta | HC-SR04 em 15 cm por 30 s, depois volta para 3 cm | o buzzer toca depois de 30 s e para ao fechar |
| RF3, temperatura | DHT22 em 10 °C por 1 min, depois volta para 5 °C | o alarme liga, o painel fica vermelho e chega um alerta; normaliza ao voltar |
| RF4, fila offline | Botão azul (simula a queda do Wi-Fi) por alguns minutos, depois aperta de novo | as leituras do período aparecem no D1 com `atraso_s` > 60 |
| Silenciar | Com um alarme ativo, colar a chave no painel e clicar em Silenciar | "Executado" no painel; com a chave errada, "Recusado: assinatura invalida" |
| ESP32 offline | Botão azul e esperar uns 45 s | o painel mostra "ESP32 offline" (LWT) e bloqueia o comando |
| Ataques ao Worker | Colar o `worker/teste_ataques.js` no console da página do Worker | só o envio válido grava; os 5 ataques dão 401 e ficam na tabela `tentativas` |

## Segurança, em resumo

- **TLS em todos os caminhos.** MQTT na porta 8883 e HTTPS para o Worker, com a CA conferida (`setCACert`, nenhum `setInsecure`). O painel usa `wss://` e `https://`.
- **Histórico protegido.** O `POST /insert` exige API key, assinatura HMAC do corpo, envio de no máximo 60 s atrás e assinatura nunca repetida (anti-replay). Os dados também são validados. Toda recusa vai para a tabela `tentativas`.
- **Comando assinado.** O painel envia `silenciar|ts|assinatura`, e o ESP32 recusa assinatura errada, comando com mais de 30 s ou repetido. O comando só cala o buzzer por 10 min: o alarme continua ativo.
- **Segredos fora do GitHub.** O `config.h` está no `.gitignore`, e no Worker as chaves são Secrets.
- **Limitação conhecida.** O broker `test.mosquitto.org` é público: quem assina os tópicos lê a telemetria, e alguém poderia publicar uma telemetria falsa no painel. O alarme (decidido no ESP32) e o histórico (só aceita dado assinado) não são afetados. Num produto, o broker seria privado, com permissão por tópico.
