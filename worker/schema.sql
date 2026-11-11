-- ================================================================
--  FrioSeguro - tabelas do banco D1 (Etapa 5 do caderno)
--  Rode este arquivo inteiro no Console do D1.
--
--  As datas aparecem em texto, no horario de Brasilia (ex.: 10/11/2026 14:56:54).
--  O horario tambem fica guardado como numero (ts_...), no fim da linha:
--  e' ele que o Worker usa para buscar "as ultimas 24 h" e fazer o resumo.
-- ================================================================

-- Uma linha por leitura do ESP32 (a cada 20 s)
CREATE TABLE IF NOT EXISTS leituras (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  data_leitura  TEXT    NOT NULL,   -- quando o ESP32 leu (Brasilia)
  data_recebido TEXT    NOT NULL,   -- quando o Worker gravou (Brasilia)
  atraso_s      INTEGER NOT NULL,   -- recebido - leitura; acima de 60 = veio da fila
  disp          TEXT    NOT NULL,   -- 'geladeira01'
  temp_c        REAL,               -- NULL se o sensor falhou
  umid_pct      REAL,
  porta         TEXT    NOT NULL,   -- aberta | fechada | desconhecida
  alarme        TEXT    NOT NULL,   -- nenhum | temperatura | porta | falha_sensor
  ts_leitura    INTEGER NOT NULL,   -- mesmo horario da leitura, em numero
  ts_recebido   INTEGER NOT NULL,
  UNIQUE (disp, ts_leitura)         -- a mesma leitura nunca entra duas vezes
);

-- Toda requisicao recusada pelo Worker (evidencia da Etapa 7)
CREATE TABLE IF NOT EXISTS tentativas (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  data      TEXT    NOT NULL,       -- Brasilia
  motivo    TEXT    NOT NULL,
  ip        TEXT,                   -- dado pessoal: fica so 30 dias
  timestamp INTEGER NOT NULL
);

-- Assinaturas ja aceitas nos ultimos 5 min (anti-replay)
CREATE TABLE IF NOT EXISTS assinaturas_usadas (
  assinatura TEXT    PRIMARY KEY,
  timestamp  INTEGER NOT NULL
);
