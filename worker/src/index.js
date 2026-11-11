// ================================================================
//  FrioSeguro - Worker da Cloudflare (o "porteiro" do historico)
//
//  Rotas:
//    POST /insert      o ESP32 grava leituras (API key + HMAC + anti-replay)
//    GET  /list        leituras das ultimas N horas (grafico do painel),
//                      com data e hora de Brasilia e o atraso de cada uma
//    GET  /resumo      resumo por hora (consulta da Etapa 5)
//    GET  /tentativas  ultimas requisicoes recusadas (so com a API key)
//    GET  /            teste rapido: responde "ok"
//
//  Segredos (painel da Cloudflare: Worker > Settings > Variables and Secrets,
//  tipo "Secret"):
//    API_KEY     igual ao API_KEY do config.h
//    CHAVE_HMAC  igual ao CHAVE_HMAC do config.h e do painel
// ================================================================

const JANELA_S = 60;              // corpo assinado vale no maximo 60 s
const IDADE_MAX_LEITURA_S = 3 * 3600;   // a fila do ESP32 guarda no maximo 2 h
const MAX_LEITURAS_POR_POST = 100;
const PORTAS = ["aberta", "fechada", "desconhecida"];
const ALARMES = ["nenhum", "temperatura", "porta", "falha_sensor"];

const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type, X-API-Key, X-Assinatura",
};

const agora = () => Math.floor(Date.now() / 1000);

function json(dados, status = 200) {
  return new Response(JSON.stringify(dados), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8", ...CORS },
  });
}

// Guarda a recusa na tabela "tentativas" e responde com o codigo HTTP
async function recusar(env, request, status, motivo) {
  const ip = request.headers.get("CF-Connecting-IP") || "desconhecido";
  try {
    const t = agora();
    await env.DB.prepare(
      "INSERT INTO tentativas (data, motivo, ip, timestamp) VALUES (strftime('%d/%m/%Y %H:%M:%S', ?, 'unixepoch', '-3 hours'), ?, ?, ?)"
    )
      .bind(t, motivo, ip, t)
      .run();
  } catch (e) {
    // se nem o registro funcionar, a recusa continua valendo
  }
  return json({ ok: false, erro: motivo, agora: agora() }, status);
}

// Compara percorrendo tudo, para o tempo de resposta nao dar pistas
function mesmoTexto(a, b) {
  const n = Math.max(a.length, b.length);
  let dif = a.length ^ b.length;
  for (let i = 0; i < n; i++) dif |= (a.charCodeAt(i) || 0) ^ (b.charCodeAt(i) || 0);
  return dif === 0;
}

// HMAC-SHA256 do texto com a CHAVE_HMAC, em hexadecimal (igual ao ESP32)
async function hmacHex(chave, texto) {
  const enc = new TextEncoder();
  const k = await crypto.subtle.importKey("raw", enc.encode(chave), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const assinatura = await crypto.subtle.sign("HMAC", k, enc.encode(texto));
  return [...new Uint8Array(assinatura)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// Confere uma leitura. Devolve o problema encontrado, ou null se estiver ok.
function problemaNaLeitura(l, t) {
  if (typeof l !== "object" || l === null) return "leitura nao e' um objeto";
  if (!Number.isInteger(l.ts_leitura)) return "ts_leitura invalido";
  if (l.ts_leitura > t + JANELA_S) return "ts_leitura no futuro";
  if (l.ts_leitura < t - IDADE_MAX_LEITURA_S) return "ts_leitura antigo demais";
  if (l.temp_c !== null && !(typeof l.temp_c === "number" && l.temp_c >= -40 && l.temp_c <= 80))
    return "temp_c fora de -40 a 80";
  if (l.umid_pct !== null && !(typeof l.umid_pct === "number" && l.umid_pct >= 0 && l.umid_pct <= 100))
    return "umid_pct fora de 0 a 100";
  if (!PORTAS.includes(l.porta)) return "porta invalida";
  if (!ALARMES.includes(l.alarme)) return "alarme invalido";
  return null;
}

// ---------- POST /insert ----------
async function inserir(request, env) {
  if (!env.API_KEY || !env.CHAVE_HMAC) return json({ ok: false, erro: "segredos nao cadastrados no Worker" }, 500);

  // Camada 2: API key
  const chave = request.headers.get("X-API-Key") || "";
  if (!mesmoTexto(chave, env.API_KEY)) return recusar(env, request, 401, "API key invalida");

  // Camada 3: assinatura HMAC do corpo exato que chegou
  const corpo = await request.text();
  const recebida = (request.headers.get("X-Assinatura") || "").toLowerCase();
  if (!recebida) return recusar(env, request, 401, "sem assinatura");
  const esperada = await hmacHex(env.CHAVE_HMAC, corpo);
  if (!mesmoTexto(recebida, esperada)) return recusar(env, request, 401, "assinatura invalida");

  let dados;
  try {
    dados = JSON.parse(corpo);
  } catch {
    return recusar(env, request, 400, "JSON invalido");
  }

  // Anti-replay 1: o "ts" do envelope e' a hora do ENVIO
  const t = agora();
  if (!Number.isInteger(dados.ts) || Math.abs(t - dados.ts) > JANELA_S)
    return recusar(env, request, 401, "envio atrasado ou no futuro");

  // Anti-replay 2: cada assinatura so vale uma vez
  await env.DB.prepare("DELETE FROM assinaturas_usadas WHERE timestamp < ?").bind(t - 5 * 60).run();
  try {
    await env.DB.prepare("INSERT INTO assinaturas_usadas (assinatura, timestamp) VALUES (?, ?)").bind(recebida, t).run();
  } catch {
    return recusar(env, request, 401, "replay (assinatura repetida)");
  }

  // Validacao do conteudo, mesmo com tudo autenticado
  if (typeof dados.disp !== "string" || !/^[a-z0-9_-]{1,32}$/.test(dados.disp))
    return recusar(env, request, 400, "disp invalido");
  if (!Array.isArray(dados.leituras) || dados.leituras.length === 0 || dados.leituras.length > MAX_LEITURAS_POR_POST)
    return recusar(env, request, 400, `leituras: mande de 1 a ${MAX_LEITURAS_POR_POST}`);
  for (const l of dados.leituras) {
    const problema = problemaNaLeitura(l, t);
    if (problema) return recusar(env, request, 400, problema);
  }

  // Grava tudo de uma vez; leitura repetida (mesmo disp e ts_leitura) e' ignorada
  // As datas em texto (Brasilia) sao montadas pelo proprio banco a partir do numero
  const inserir = env.DB.prepare(
    `INSERT OR IGNORE INTO leituras
       (data_leitura, data_recebido, atraso_s, disp, temp_c, umid_pct, porta, alarme, ts_leitura, ts_recebido)
     VALUES (strftime('%d/%m/%Y %H:%M:%S', ?1, 'unixepoch', '-3 hours'),
             strftime('%d/%m/%Y %H:%M:%S', ?2, 'unixepoch', '-3 hours'),
             ?2 - ?1, ?3, ?4, ?5, ?6, ?7, ?1, ?2)`
  );
  const resultado = await env.DB.batch(
    dados.leituras.map((l) => inserir.bind(l.ts_leitura, t, dados.disp, l.temp_c, l.umid_pct, l.porta, l.alarme))
  );
  const gravadas = resultado.reduce((soma, r) => soma + (r.meta.changes || 0), 0);
  // "agora" = relogio do Worker: o ESP32 usa para corrigir o proprio relogio
  return json({ ok: true, recebidas: dados.leituras.length, gravadas, agora: t });
}

// ---------- GET /list?disp=geladeira01&horas=24 ----------
async function listar(url, env) {
  const disp = url.searchParams.get("disp") || "geladeira01";
  const horas = Math.min(Math.max(parseInt(url.searchParams.get("horas") || "24", 10) || 24, 1), 168);
  const { results } = await env.DB.prepare(
    `SELECT data_leitura, data_recebido, atraso_s, temp_c, umid_pct, porta, alarme, ts_leitura, ts_recebido
     FROM leituras
     WHERE disp = ? AND ts_leitura >= ?
     ORDER BY ts_leitura`
  )
    .bind(disp, agora() - horas * 3600)
    .all();
  return json(results);
}

// ---------- GET /resumo?disp=geladeira01&horas=24 ----------
// Pergunta: hora a hora, minima, maxima, media, minutos fora de 2-8 C e se faltou leitura
async function resumo(url, env) {
  const disp = url.searchParams.get("disp") || "geladeira01";
  const horas = Math.min(Math.max(parseInt(url.searchParams.get("horas") || "24", 10) || 24, 1), 168);
  const { results } = await env.DB.prepare(
    `SELECT
       strftime('%d/%m/%Y %H:00', ts_leitura, 'unixepoch', '-3 hours') AS hora,
       COUNT(*)                                                       AS leituras,
       ROUND(MIN(temp_c), 1)                                          AS min_c,
       ROUND(MAX(temp_c), 1)                                          AS max_c,
       ROUND(AVG(temp_c), 1)                                          AS media_c,
       SUM(CASE WHEN temp_c < 2 OR temp_c > 8 THEN 20 ELSE 0 END) / 60.0 AS min_fora_da_faixa,
       SUM(CASE WHEN temp_c IS NULL THEN 1 ELSE 0 END)                AS falhas_sensor
     FROM leituras
     WHERE disp = ? AND ts_leitura >= ?
     GROUP BY hora
     ORDER BY MIN(ts_leitura)`
  )
    .bind(disp, agora() - horas * 3600)
    .all();
  return json(results);
}

// ---------- GET /tentativas (tem IP: so com a API key) ----------
async function tentativas(request, env) {
  const chave = request.headers.get("X-API-Key") || "";
  if (!env.API_KEY || !mesmoTexto(chave, env.API_KEY)) return json({ ok: false, erro: "precisa da API key" }, 401);
  const { results } = await env.DB.prepare(
    "SELECT data, motivo, ip FROM tentativas ORDER BY id DESC LIMIT 50"
  ).all();
  return json(results);
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    try {
      if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: CORS });
      if (request.method === "POST" && url.pathname === "/insert") return await inserir(request, env);
      if (request.method === "GET" && url.pathname === "/list") return await listar(url, env);
      if (request.method === "GET" && url.pathname === "/resumo") return await resumo(url, env);
      if (request.method === "GET" && url.pathname === "/tentativas") return await tentativas(request, env);
      if (request.method === "GET" && url.pathname === "/") return new Response("FrioSeguro Worker: ok", { headers: CORS });
      return json({ ok: false, erro: "rota nao encontrada" }, 404);
    } catch (e) {
      return json({ ok: false, erro: "erro interno" }, 500);
    }
  },

  // Limpeza diaria (Cron Trigger do Worker, "0 6 * * *"): retencao da Etapa 5
  async scheduled(event, env) {
    const t = agora();
    await env.DB.batch([
      env.DB.prepare("DELETE FROM leituras WHERE ts_leitura < ?").bind(t - 365 * 24 * 3600),
      env.DB.prepare("DELETE FROM tentativas WHERE timestamp < ?").bind(t - 30 * 24 * 3600),
      env.DB.prepare("DELETE FROM assinaturas_usadas WHERE timestamp < ?").bind(t - 5 * 60),
    ]);
  },
};
