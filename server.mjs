// VitaWA bridge v3 — conecta WhatsApp (via Baileys) con la PS Vita.
//
// La Vita habla con este server por un canal cifrado (channel.mjs): un solo
// POST /x con la peticion adentro. Se empareja escaneando un QR con la camara.
//
// Respuestas en texto plano: una fila por linea, campos separados por TAB.
//  - Un salto de linea dentro de un campo viaja como "\n" literal (2 caracteres).
//  - Cada emoji viaja como  \x1E<nombre twemoji>\x1F  (ej. \x1E1f44d-1f3fd\x1F).
// Las horas ya vienen formateadas en la zona horaria de la Vita.
//
// Desde esta misma PC (localhost) tambien se puede abrir http://localhost:8787
// para ver el QR de emparejamiento, y /v1/... sin cifrar para depurar.

import http from 'node:http';
import os from 'node:os';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import pino from 'pino';
import QRCode from 'qrcode';
import sharp from 'sharp';
import emojiRegex from 'emoji-regex';
import { parsePhoneNumberFromString } from 'libphonenumber-js';
import makeWASocket, {
  Browsers,
  BufferJSON,
  DisconnectReason,
  downloadMediaMessage,
  downloadContentFromMessage,
  fetchLatestBaileysVersion,
  jidNormalizedUser,
  useMultiFileAuthState,
} from 'baileys';
import { loadKey, pairingText, openRequest, sealResponse } from './channel.mjs';
import { oggToMulaw, mulawToOgg } from './audio.mjs';

const require = createRequire(import.meta.url);
const HERE = path.dirname(fileURLToPath(import.meta.url));
const DATA = process.env.DATA_DIR || path.join(HERE, 'data');
const AUTH_DIR = path.join(DATA, 'auth');
const STORE_FILE = path.join(DATA, 'store.json');
const TWEMOJI_DIR = path.dirname(require.resolve('@twemoji/svg/package.json'));
const PORT = Number(process.env.PORT || 8787);
const VERSION = '3.5.0';

const MAX_MSGS_PER_CHAT = 100;
const MAX_STICKERS = 32;
const STATUS_TTL_S = 24 * 3600;
const LONG_POLL_MAX_S = 30;
const MAX_UPLOAD = 8 * 1024 * 1024;

fs.mkdirSync(DATA, { recursive: true });
const logger = pino({ level: process.env.WA_LOG || 'warn' });
function log(...a) {
  console.log(new Date().toISOString().slice(11, 19), ...a);
}

// ---------- emparejamiento ----------
const KEY = loadKey(DATA, process.argv.includes('--nuevo-codigo') || process.env.PAIR_RESET === '1');

function lanIps() {
  return Object.values(os.networkInterfaces())
    .flat()
    .filter((i) => i && i.family === 'IPv4' && !i.internal)
    .map((i) => i.address);
}
// En Render la dirección pública llega sola en RENDER_EXTERNAL_URL (https://xxx.onrender.com)
const PUBLIC_URL = (process.env.PUBLIC_URL || process.env.RENDER_EXTERNAL_URL || `http://${lanIps()[0] || '127.0.0.1'}:${PORT}`).replace(/\/+$/, '');
// Para abrir la página del bridge (QR de emparejamiento) desde fuera del servidor: /?k=ADMIN_TOKEN
const ADMIN_TOKEN = process.env.ADMIN_TOKEN || process.env.VITAWA_TOKEN || '';
const PAIR_TEXT = pairingText(PUBLIC_URL, KEY);

// ---------- estado de conexion ----------
const st = {
  seq: 1,            // sube con cualquier cambio (la Vita hace long-poll sobre esto)
  statusVer: 1,      // sube cuando cambian los estados
  state: 'starting', // starting | qr | connecting | open | logged_out
  qr: null,          // { size, rows, raw, at }
  me: null,          // { jid, lid, number, name }
};
let waiters = [];
function bump() {
  st.seq++;
  const w = waiters;
  waiters = [];
  for (const fn of w) fn();
}
function setState(s) {
  if (st.state === s) return;
  st.state = s;
  log(`estado -> ${s}`);
  bump();
}

// ---------- store ----------
// chats:    jid -> { jid, name, unread, ts, ver }
// msgs:     jid -> [Entry]   Entry = { id, raw, ts, fromMe, sender, push, ack, starred, reactions, revoked, edited }
// status:   posterJid -> [Entry]   (mis estados bajo la clave 'me')
// contacts: jid -> { name, notify, verifiedName }
// lidmap:   lid -> pn   (WhatsApp ahora identifica a muchos contactos con un "LID" en vez del numero)
// stickers: { fav, recent, received }
const EMPTY_STORE = () => ({ v: 3, chats: {}, msgs: {}, status: {}, contacts: {}, lidmap: {}, stickers: { fav: [], recent: [], received: [] }, tz: null });
let store = EMPTY_STORE();
try {
  const loaded = JSON.parse(fs.readFileSync(STORE_FILE, 'utf8'), BufferJSON.reviver);
  if (loaded && (loaded.v === 2 || loaded.v === 3)) store = { ...EMPTY_STORE(), ...loaded, v: 3 };
  else if (loaded && !Array.isArray(loaded.chats)) {
    for (const c of Object.values(loaded.chats || {})) if (c?.jid) store.chats[c.jid] = { jid: c.jid, name: c.name, unread: c.unread || 0, ts: c.ts || 0, ver: 1 };
    if (loaded.contacts && !Array.isArray(loaded.contacts)) store.contacts = loaded.contacts;
  }
  // (un store.json de otro formato —p. ej. del bridge viejo de VitaWA— se ignora; la sesión de WhatsApp sigue en auth/)
} catch {}

let saveTimer = null;
function saveSoon() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(saveNow, 2000);
}
function saveNow() {
  try {
    fs.writeFileSync(STORE_FILE + '.tmp', JSON.stringify(store, BufferJSON.replacer));
    fs.renameSync(STORE_FILE + '.tmp', STORE_FILE);
  } catch (e) {
    log('no pude guardar el store:', e.message);
  }
}

const isGroup = (jid) => !!jid?.endsWith('@g.us');
const isLid = (jid) => !!jid?.endsWith('@lid');
const isPn = (jid) => !!jid?.endsWith('@s.whatsapp.net');
const skipJid = (jid) => !jid || jid.endsWith('@newsletter') || (jid.endsWith('@broadcast') && jid !== 'status@broadcast');
const myJid = () => st.me?.jid || '';
function isMe(jid) {
  if (!jid) return false;
  const j = jidNormalizedUser(jid);
  return j === myJid() || (!!st.me?.lid && j === st.me.lid) || canon(j) === myJid();
}

// ---------- LID <-> numero ----------
function rememberLid(lid, pn) {
  if (!lid || !pn) return;
  lid = jidNormalizedUser(lid);
  pn = jidNormalizedUser(pn);
  if (!isLid(lid) || !isPn(pn) || store.lidmap[lid] === pn) return;
  store.lidmap[lid] = pn;
  mergeChat(lid, pn);
  saveSoon();
}
function canon(jid) {
  if (!jid) return jid;
  const j = jidNormalizedUser(jid);
  return (isLid(j) && store.lidmap[j]) || j;
}
function lidOf(pn) {
  for (const [l, p] of Object.entries(store.lidmap)) if (p === pn) return l;
  return null;
}
// si un chat existia con el LID y ahora sabemos el numero, se juntan
function mergeChat(lid, pn) {
  const a = store.chats[lid];
  if (!a) return;
  const b = (store.chats[pn] ||= { jid: pn, name: a.name, unread: 0, ts: 0, ver: 1 });
  b.unread += a.unread || 0;
  b.ts = Math.max(b.ts || 0, a.ts || 0);
  b.ver++;
  const msgs = [...(store.msgs[pn] || []), ...(store.msgs[lid] || [])].sort((x, y) => x.ts - y.ts);
  store.msgs[pn] = msgs.slice(-MAX_MSGS_PER_CHAT);
  delete store.chats[lid];
  delete store.msgs[lid];
  bump();
}
function learnFromKey(key) {
  if (!key) return;
  if (key.remoteJidAlt) {
    if (isLid(key.remoteJid)) rememberLid(key.remoteJid, key.remoteJidAlt);
    else if (isLid(key.remoteJidAlt)) rememberLid(key.remoteJidAlt, key.remoteJid);
  }
  if (key.participantAlt) {
    if (isLid(key.participant)) rememberLid(key.participant, key.participantAlt);
    else if (isLid(key.participantAlt)) rememberLid(key.participantAlt, key.participant);
  }
}

// ---------- nombres ----------
function formatNumber(jid) {
  const digits = String(jid || '').split('@')[0].split(':')[0];
  if (!/^\d{6,}$/.test(digits)) return '';
  const p = parsePhoneNumberFromString('+' + digits);
  return p ? p.formatInternational() : '+' + digits;
}
function contactOf(jid) {
  const c = canon(jid);
  const l = isPn(c) ? lidOf(c) : null;
  return { ...(l && store.contacts[l]), ...store.contacts[jid], ...store.contacts[c] };
}
// nombre guardado en tu agenda > nombre verificado (empresas) > nombre de perfil > numero
function nameFor(jid, { pushFallback = true } = {}) {
  if (!jid) return '';
  if (isMe(jid)) return 'Tú';
  const c = contactOf(jid);
  if (c.name) return c.name;
  if (c.verifiedName) return c.verifiedName;
  if (isGroup(jid)) return store.chats[jid]?.name || 'Grupo';
  const num = formatNumber(canon(jid));
  if (num) return num;
  if (pushFallback && c.notify) return '~ ' + c.notify;
  return 'Contacto';
}
function chatName(jid) {
  const c = store.chats[jid];
  if (isGroup(jid)) return c?.name || groupMeta[jid]?.subject || 'Grupo';
  const saved = contactOf(jid).name;
  return saved || c?.name || nameFor(jid);
}
function colorIdx(jid) {
  return crypto.createHash('md5').update(String(canon(jid))).digest()[0] % 8;
}
function chatFor(jid) {
  return (store.chats[jid] ||= { jid, name: null, unread: 0, ts: 0, ver: 1 });
}
function touchChat(jid) {
  const c = chatFor(jid);
  c.ver = (c.ver || 0) + 1;
}

// ---------- texto ----------
const EMOJI_RE = emojiRegex();
const emojiNameCache = new Map();
function emojiName(seq) {
  if (emojiNameCache.has(seq)) return emojiNameCache.get(seq);
  const codes = [...seq].map((c) => c.codePointAt(0).toString(16));
  const tries = [codes.join('-'), codes.filter((c) => c !== 'fe0f').join('-'), codes[0]];
  let found = null;
  for (const t of tries) {
    if (t && fs.existsSync(path.join(TWEMOJI_DIR, t + '.svg'))) {
      found = t;
      break;
    }
  }
  emojiNameCache.set(seq, found);
  return found;
}
function out(s) {
  return String(s ?? '')
    .replace(/[\t\r\x1E\x1F]/g, ' ')
    .replace(/\n/g, '\\n')
    .replace(EMOJI_RE, (m) => {
      const n = emojiName(m);
      return n ? `\x1E${n}\x1F` : '';
    })
    // la fuente de la Vita solo entiende el plano basico de Unicode
    .replace(/[\u{10000}-\u{10FFFF}]/gu, '')
    .replace(/(\x1E[0-9a-f-]+\x1F)|[️‍]/g, (m, keep) => keep || '');
}
const row = (...f) => f.map(out).join('\t') + '\n';

// ---------- fechas (zona horaria de la Vita) ----------
// La Vita manda su diferencia con UTC en minutos; si no, se usa la de esta PC.
let tzMin = typeof store.tz === 'number' ? store.tz : -new Date().getTimezoneOffset();
function setTz(v) {
  const n = Number(v);
  if (Number.isFinite(n) && Math.abs(n) <= 14 * 60 && n !== tzMin) {
    tzMin = n;
    store.tz = n;
    saveSoon();
  }
}
const local = (ts) => new Date(ts * 1000 + tzMin * 60000); // usar getUTC*
const DIAS = ['Dom', 'Lun', 'Mar', 'Mié', 'Jue', 'Vie', 'Sáb'];
const MESES = ['ene', 'feb', 'mar', 'abr', 'may', 'jun', 'jul', 'ago', 'sep', 'oct', 'nov', 'dic'];
const pad = (n) => String(n).padStart(2, '0');
function dayDiff(ts) {
  const d = local(ts);
  const now = local(Date.now() / 1000);
  const a = Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate());
  const b = Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate());
  return Math.round((b - a) / 86400000);
}
const hhmm = (ts) => {
  const d = local(ts);
  return `${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}`;
};
function listTime(ts) {
  if (!ts) return '';
  const dd = dayDiff(ts);
  const d = local(ts);
  if (dd <= 0) return hhmm(ts);
  if (dd === 1) return 'Ayer';
  if (dd < 7) return DIAS[d.getUTCDay()];
  return `${pad(d.getUTCDate())}/${pad(d.getUTCMonth() + 1)}/${String(d.getUTCFullYear()).slice(2)}`;
}
function dayLabel(ts) {
  const dd = dayDiff(ts);
  const d = local(ts);
  if (dd <= 0) return 'HOY';
  if (dd === 1) return 'AYER';
  return `${DIAS[d.getUTCDay()].toUpperCase()} ${d.getUTCDate()} ${MESES[d.getUTCMonth()].toUpperCase()}`;
}
function agoLabel(ts) {
  const s = Math.max(0, Date.now() / 1000 - ts);
  if (s < 60) return 'ahora';
  if (s < 3600) return `hace ${Math.floor(s / 60)} min`;
  if (dayDiff(ts) <= 0) return `hace ${Math.floor(s / 3600)} h`;
  if (dayDiff(ts) === 1) return `ayer a las ${hhmm(ts)}`;
  const d = local(ts);
  return `el ${d.getUTCDate()} ${MESES[d.getUTCMonth()]}`;
}
const dur = (s) => (s ? `${Math.floor(s / 60)}:${pad(Math.floor(s % 60))}` : '');

// ---------- interpretar mensajes ----------
function unwrap(message) {
  let msg = message;
  for (let i = 0; i < 5 && msg; i++) {
    const inner =
      msg.ephemeralMessage?.message ||
      msg.viewOnceMessage?.message ||
      msg.viewOnceMessageV2?.message ||
      msg.viewOnceMessageV2Extension?.message ||
      msg.documentWithCaptionMessage?.message ||
      msg.editedMessage?.message;
    if (!inner) break;
    msg = inner;
  }
  return msg;
}

// { kind, text, dur, ctx }  kind: text|image|video|gif|audio|voice|sticker|doc|location|contact|poll
function describeMessage(message) {
  const m = unwrap(message);
  if (!m) return null;
  if (m.conversation != null) return { kind: 'text', text: m.conversation };
  if (m.extendedTextMessage) return { kind: 'text', text: m.extendedTextMessage.text || '', ctx: m.extendedTextMessage.contextInfo, bg: m.extendedTextMessage.backgroundArgb };
  if (m.imageMessage) return { kind: 'image', text: m.imageMessage.caption || '', ctx: m.imageMessage.contextInfo };
  if (m.videoMessage) return { kind: m.videoMessage.gifPlayback ? 'gif' : 'video', text: m.videoMessage.caption || '', dur: m.videoMessage.seconds, ctx: m.videoMessage.contextInfo };
  if (m.audioMessage) return { kind: m.audioMessage.ptt ? 'voice' : 'audio', text: '', dur: m.audioMessage.seconds, ctx: m.audioMessage.contextInfo };
  if (m.stickerMessage) return { kind: 'sticker', text: '', ctx: m.stickerMessage.contextInfo };
  if (m.documentMessage) return { kind: 'doc', text: m.documentMessage.fileName || m.documentMessage.title || 'Documento', ctx: m.documentMessage.contextInfo };
  if (m.locationMessage || m.liveLocationMessage) return { kind: 'location', text: m.locationMessage?.name || m.locationMessage?.address || '' };
  if (m.contactMessage) return { kind: 'contact', text: m.contactMessage.displayName || '' };
  if (m.contactsArrayMessage) return { kind: 'contact', text: m.contactsArrayMessage.displayName || '' };
  if (m.pollCreationMessage || m.pollCreationMessageV2 || m.pollCreationMessageV3) {
    const p = m.pollCreationMessage || m.pollCreationMessageV2 || m.pollCreationMessageV3;
    return { kind: 'poll', text: p.name || '' };
  }
  return null;
}

const KIND_LABEL = {
  image: 'Foto', video: 'Video', gif: 'GIF', audio: 'Audio', voice: 'Nota de voz',
  sticker: 'Sticker', doc: 'Documento', location: 'Ubicación', contact: 'Contacto', poll: 'Encuesta',
};
function summary(d) {
  if (!d) return '';
  if (d.kind === 'text') return d.text;
  const label = KIND_LABEL[d.kind] || 'Mensaje';
  const extra = d.dur ? dur(d.dur) : d.text;
  return extra ? `${label} · ${extra}` : label;
}
function entryInfo(e) {
  if (e.revoked) return { kind: 'deleted', text: e.fromMe ? 'Eliminaste este mensaje' : 'Se eliminó este mensaje' };
  const d = describeMessage(e.raw.message) || { kind: 'other', text: 'Mensaje no compatible' };
  if (e.edited != null) d.text = e.edited;
  return d;
}

// ---------- guardar mensajes ----------
function findEntry(jid, id) {
  // Stickers recientes de tu teléfono (llegan al vincular): se arma un mensaje
  // "de mentira" con su metadata para poder descargarlos igual que cualquier otro.
  if (jid === '@phone') {
    const p = (store.stickers.phone || []).find((x) => x.id === id);
    if (!p) return undefined;
    const b = (v) => (v ? Buffer.from(v, 'base64') : undefined);
    const sm = { url: p.meta.url, directPath: p.meta.directPath, mediaKey: b(p.meta.mediaKey), fileSha256: b(p.meta.fileSha256), fileEncSha256: b(p.meta.fileEncSha256), mimetype: 'image/webp' };
    return { id, raw: { key: { id, remoteJid: '@phone' }, message: { stickerMessage: sm } }, phone: true };
  }
  return (store.msgs[canon(jid)] || store.msgs[jid] || []).find((x) => x.id === id);
}
function applyReaction(react, reactorJid) {
  const key = react.key;
  if (!key) return false;
  const e = findEntry(key.remoteJid, key.id);
  if (!e) return false;
  e.reactions ||= {};
  const text = react.text || '';
  const who = canon(reactorJid);
  if (text) e.reactions[who] = text;
  else delete e.reactions[who];
  touchChat(canon(key.remoteJid));
  return true;
}
function rememberSticker(list, jid, id) {
  const arr = (store.stickers[list] ||= []);
  const i = arr.findIndex((s) => s.id === id);
  if (i >= 0) arr.splice(i, 1);
  arr.unshift({ jid, id });
  if (arr.length > MAX_STICKERS) arr.length = MAX_STICKERS;
}

function addMessage(m, { live }) {
  learnFromKey(m.key);
  const rawJid = m.key?.remoteJid;
  if (skipJid(rawJid) || !m.message) return false;
  const jid = rawJid === 'status@broadcast' ? rawJid : canon(rawJid);
  const fromMe = !!m.key.fromMe;
  const sender = fromMe ? myJid() : canon(m.key.participant || rawJid);
  const msg = unwrap(m.message);

  if (m.pushName && !fromMe) {
    const c = (store.contacts[sender] ||= {});
    if (c.notify !== m.pushName) c.notify = m.pushName;
  }

  if (msg?.reactionMessage) return applyReaction(msg.reactionMessage, sender);

  const proto = msg?.protocolMessage;
  if (proto) {
    const target = proto.key && findEntry(proto.key.remoteJid || rawJid, proto.key.id);
    if (!target) return false;
    if (proto.type === 0) target.revoked = true; // REVOKE
    if (proto.type === 14 && proto.editedMessage) target.edited = describeMessage(proto.editedMessage)?.text ?? target.edited;
    touchChat(jid);
    return true;
  }

  const d = describeMessage(m.message);
  if (!d) return false;
  const ts = Number(m.messageTimestamp || Date.now() / 1000);

  if (jid === 'status@broadcast') {
    if (Date.now() / 1000 - ts > STATUS_TTL_S) return false;
    const poster = fromMe ? 'me' : sender;
    const list = (store.status[poster] ||= []);
    if (list.some((x) => x.id === m.key.id)) return false;
    list.push({ id: m.key.id, raw: m, ts, fromMe, sender, seen: fromMe });
    list.sort((a, b) => a.ts - b.ts);
    st.statusVer++;
    return true;
  }

  const list = (store.msgs[jid] ||= []);
  if (list.some((x) => x.id === m.key.id)) return false;
  list.push({ id: m.key.id, raw: m, ts, fromMe, sender, push: m.pushName || '', ack: m.status ?? (fromMe ? 2 : 0), starred: !!m.starred, reactions: {} });
  list.sort((a, b) => a.ts - b.ts);
  if (list.length > MAX_MSGS_PER_CHAT) list.splice(0, list.length - MAX_MSGS_PER_CHAT);

  const chat = chatFor(jid);
  if (ts >= chat.ts) chat.ts = ts;
  if (live && !fromMe) {
    chat.unread++;
    if (typing[jid]?.has(sender)) setTyping(jid, sender, null);   // ya mandó lo que escribía
    notify('msg', jid, sender, '', summary(d));
  }
  chat.ver++;

  if (d.kind === 'sticker') rememberSticker(fromMe ? 'recent' : 'received', jid, m.key.id);
  return true;
}

function pruneStatuses() {
  const now = Date.now() / 1000;
  let changed = false;
  for (const [k, list] of Object.entries(store.status)) {
    const keep = list.filter((x) => now - x.ts < STATUS_TTL_S);
    if (keep.length !== list.length) changed = true;
    if (keep.length) store.status[k] = keep;
    else delete store.status[k];
  }
  if (changed) {
    st.statusVer++;
    bump();
  }
}
setInterval(pruneStatuses, 10 * 60 * 1000);

// ---------- presencia y grupos ----------
const presence = {};  // jid -> { state, lastSeen, who }

// ---------- "escribiendo…" ----------
// WhatsApp manda composing/recording mientras el otro escribe, pero el 'paused'
// final NO siempre llega (cerró la app, perdió señal). Por eso cada participante
// tiene su propio temporizador de 10 s: si no llega otro evento, se apaga solo.
// En grupos se lleva por persona (antes, el 'paused' de uno apagaba a todos).
const TYPING_MS = 10 * 1000;
const typing = {};    // jid del chat -> Map(participante -> { kind, timer })
function setTyping(jid, who, kind) {
  const m = (typing[jid] ||= new Map());
  const prev = m.get(who);
  if (prev) clearTimeout(prev.timer);
  if (kind) m.set(who, { kind, timer: setTimeout(() => setTyping(jid, who, null), TYPING_MS) });
  else m.delete(who);
  const changed = !prev || !kind || prev.kind !== kind;
  if (!m.size) delete typing[jid];
  if (changed) {
    if (store.chats[jid]) touchChat(jid);
    bump();
  }
}
function typingLabel(jid) {
  const m = typing[jid];
  if (!m?.size) return '';
  const list = [...m.entries()];
  const rec = list.some(([, v]) => v.kind === 'recording');
  if (isGroup(jid)) {
    if (list.length > 1) return `${list.length} personas ${rec ? 'grabando' : 'escribiendo'}…`;
    return `${nameFor(list[0][0])} está ${rec ? 'grabando audio' : 'escribiendo'}…`;
  }
  return rec ? 'grabando audio…' : 'escribiendo…';
}

// ---------- notificaciones (banner + campanita en la Vita) ----------
//  msg   = te escribieron (y no fuiste tú)
//  react = alguien reaccionó a un mensaje tuyo
//  slike = alguien reaccionó a tu estado
let notifSeq = 0;
const notifs = [];
const notifDedupe = new Map();
function notify(kind, jid, who, emoji, body) {
  const k = [kind, jid, who, emoji, body].join('|');
  const now = Date.now();
  if ((notifDedupe.get(k) || 0) > now - 60 * 1000) return;
  notifDedupe.set(k, now);
  if (notifDedupe.size > 500) for (const [kk, t] of notifDedupe) if (t < now - 120000) notifDedupe.delete(kk);
  notifs.push({ seq: ++notifSeq, kind, jid, who, emoji, body: String(body || '').slice(0, 160), ts: Math.floor(now / 1000) });
  if (notifs.length > 50) notifs.shift();
  bump();
}

// ---------- presencia: suscribirse mientras haya una Vita mirando ----------
let lastVitaSeen = 0;
let markedAvailable = false;
async function subscribeTop() {
  if (!sock || st.state !== 'open') return;
  const top = Object.values(store.chats)
    .filter((c) => c.ts > 0 && !isGroup(c.jid) && !skipJid(c.jid) && !isMe(c.jid))
    .sort((a, b) => b.ts - a.ts).slice(0, 20);
  for (const c of top) {
    for (const j of [c.jid, lidOf(c.jid)]) if (j) await sock.presenceSubscribe(j).catch(() => {});
    await new Promise((r) => setTimeout(r, 120));
  }
}
setInterval(async () => {
  if (!sock || st.state !== 'open') return;
  const active = Date.now() - lastVitaSeen < 90 * 1000;
  if (active) {
    // "en línea" mientras usas la Vita → WhatsApp nos manda las presencias
    await sock.sendPresenceUpdate('available').catch(() => {});
    if (!markedAvailable) { markedAvailable = true; subscribeTop(); }
  } else if (markedAvailable) {
    markedAvailable = false;
    await sock.sendPresenceUpdate('unavailable').catch(() => {}); // el teléfono vuelve a notificar
  }
}, 30 * 1000);
setInterval(() => { if (markedAvailable) subscribeTop(); }, 4 * 60 * 1000);
const groupMeta = {}; // jid -> { subject, desc, participants:[{id, admin}], at }

async function ensureGroupMeta(jid, force = false) {
  if (!isGroup(jid) || st.state !== 'open') return groupMeta[jid];
  const g = groupMeta[jid];
  if (g && !force && Date.now() - g.at < 10 * 60 * 1000) return g;
  groupMeta[jid] = { ...(g || {}), at: Date.now() };
  try {
    const md = await sock.groupMetadata(jid);
    for (const p of md.participants || []) {
      if (p.lid && p.phoneNumber) rememberLid(p.lid, p.phoneNumber);
      if (isLid(p.id) && p.phoneNumber) rememberLid(p.id, p.phoneNumber);
    }
    groupMeta[jid] = {
      subject: md.subject,
      desc: md.desc || '',
      participants: (md.participants || []).map((p) => ({ id: canon(p.phoneNumber || p.id), admin: p.admin || '' })),
      at: Date.now(),
    };
    if (md.subject && store.chats[jid] && store.chats[jid].name !== md.subject) {
      store.chats[jid].name = md.subject;
      touchChat(jid);
      saveSoon();
      bump();
    }
  } catch {}
  return groupMeta[jid];
}

let allGroups = { at: 0, list: [] };
async function participatingGroups() {
  if (Date.now() - allGroups.at < 10 * 60 * 1000 || st.state !== 'open') return allGroups.list;
  try {
    const res = await sock.groupFetchAllParticipating();
    allGroups = {
      at: Date.now(),
      list: Object.values(res).map((g) => ({
        id: g.id,
        subject: g.subject,
        members: new Set((g.participants || []).map((p) => canon(p.phoneNumber || p.id))),
      })),
    };
  } catch {}
  return allGroups.list;
}

function subtitleFor(jid) {
  const p = presence[jid];
  const tl = typingLabel(jid);
  if (tl) return tl;
  if (isGroup(jid)) {
    const parts = groupMeta[jid]?.participants;
    if (!parts?.length) return 'Grupo';
    const names = parts.filter((x) => !isMe(x.id)).map((x) => nameFor(x.id));
    const shown = names.slice(0, 4).join(', ');
    return names.length > 4 ? `${shown} y ${names.length - 4} más` : shown;
  }
  if (p?.state === 'available') return 'en línea';
  if (p?.lastSeen) return `últ. vez ${agoLabel(p.lastSeen)}`;
  return '';
}

// ---------- WhatsApp ----------
let sock = null;
let retries = 0;
let starting = false;

async function startWA() {
  if (starting) return;
  starting = true;
  try {
    const { state, saveCreds } = await useMultiFileAuthState(AUTH_DIR);
    let version;
    try {
      ({ version } = await fetchLatestBaileysVersion());
    } catch {}

    sock = makeWASocket({
      version,
      auth: state,
      logger,
      // nombres de navegador "raros" a veces hacen que el teléfono rechace la vinculación
      browser: Browsers.macOS('Chrome'),
      markOnlineOnConnect: false,
      syncFullHistory: false,
      getMessage: async (key) => findEntry(key.remoteJid, key.id)?.raw?.message || undefined,
    });

    sock.ev.on('creds.update', () => {
      saveCreds();
      const name = sock.authState?.creds?.me?.name;
      if (st.me && name && st.me.name !== name) {
        st.me.name = name;
        bump();
      }
    });

    sock.ev.on('connection.update', async (u) => {
      if (u.qr) {
        const q = QRCode.create(u.qr, { errorCorrectionLevel: 'L' });
        const size = q.modules.size;
        const rows = [];
        for (let y = 0; y < size; y++) {
          let r = '';
          for (let x = 0; x < size; x++) r += q.modules.get(y, x) ? '1' : '0';
          rows.push(r);
        }
        st.qr = { size, rows, raw: u.qr, at: Date.now() };
        st.state = 'qr';
        log('Esperando que vincules WhatsApp (el QR aparece en la Vita y en http://localhost:' + PORT + ')');
        bump();
      }
      if (u.connection === 'open') {
        retries = 0;
        st.qr = null;
        st.pairCode = null;
        const jid = jidNormalizedUser(sock.user?.id || '');
        const lid = sock.user?.lid ? jidNormalizedUser(sock.user.lid) : null;
        st.me = { jid, lid, number: jid.split('@')[0], name: sock.user?.name || sock.authState?.creds?.me?.name || '' };
        if (lid) rememberLid(lid, jid);
        setState('open');
        log(`conectado como ${st.me.name || '(sin nombre aun)'} (${formatNumber(jid)})`);
      }
      if (u.connection === 'close') {
        const code = u.lastDisconnect?.error?.output?.statusCode;
        st.pairCode = null;
        if (code === DisconnectReason.loggedOut) {
          log('sesion cerrada desde el telefono; borrando credenciales');
          fs.rmSync(AUTH_DIR, { recursive: true, force: true });
          const keepTz = store.tz;
          store = EMPTY_STORE();
          store.tz = keepTz;
          saveSoon();
          st.me = null;
          st.qr = null;
          setState('logged_out');
          setTimeout(startWA, 1000);
        } else {
          // 515 (restartRequired) es normal justo despues de escanear el QR
          const delay = code === DisconnectReason.restartRequired ? 0 : Math.min(30000, 2000 * 2 ** retries++);
          if (st.state !== 'qr') setState('connecting');
          log(`conexion cerrada (codigo ${code}); reintento en ${delay / 1000}s`);
          setTimeout(startWA, delay);
        }
      }
    });

    sock.ev.on('lid-mapping.update', (m) => {
      rememberLid(m?.lid, m?.pn);
    });

    sock.ev.on('messaging-history.set', ({ chats, contacts, messages, lidPnMappings, recentStickers }) => {
      for (const m of lidPnMappings || []) rememberLid(m.lid, m.pn);
      // stickers recientes del teléfono (scripts/patch-baileys.mjs los deja pasar)
      if (recentStickers?.length) {
        const phone = (store.stickers.phone ||= []);
        for (const sm of recentStickers) {
          if (!sm?.fileSha256 || sm.isLottie || !(sm.directPath || sm.url)) continue;
          const id = Buffer.from(sm.fileSha256).toString('hex').slice(0, 40);
          if (phone.some((x) => x.id === id)) continue;
          phone.push({ id, ts: Number(sm.lastStickerSentTs || 0), meta: {
            url: sm.url, directPath: sm.directPath, mediaKey: sm.mediaKey && Buffer.from(sm.mediaKey).toString('base64'),
            fileSha256: Buffer.from(sm.fileSha256).toString('base64'),
            fileEncSha256: sm.fileEncSha256 && Buffer.from(sm.fileEncSha256).toString('base64'),
          } });
        }
        phone.sort((a, b) => b.ts - a.ts);
        if (phone.length > 48) phone.length = 48;
        log(`stickers recientes del teléfono: ${phone.length}`);
      }
      upsertContacts(contacts || []);
      for (const c of chats || []) {
        if (skipJid(c.id) || c.id === 'status@broadcast') continue;
        const jid = canon(c.id);
        const chat = chatFor(jid);
        if (c.name) chat.name = c.name;
        if (typeof c.unreadCount === 'number') chat.unread = Math.max(0, c.unreadCount);
        const ts = Number(c.conversationTimestamp || 0);
        if (ts > chat.ts) chat.ts = ts;
      }
      for (const m of messages || []) addMessage(m, { live: false });
      saveSoon();
      bump();
    });

    function upsertContacts(list) {
      let changed = false;
      for (const c of list) {
        if (!c.id) continue;
        if (c.lid && c.phoneNumber) rememberLid(c.lid, c.phoneNumber);
        if (isLid(c.id) && c.phoneNumber) rememberLid(c.id, c.phoneNumber);
        if (isPn(c.id) && c.lid) rememberLid(c.lid, c.id);
        const key = canon(c.id);
        const prev = store.contacts[key] || {};
        const next = { ...prev };
        if (c.name) next.name = c.name;
        if (c.notify) next.notify = c.notify;
        if (c.verifiedName) next.verifiedName = c.verifiedName;
        if (c.imgUrl !== undefined) avatarCache.forEach((v, k) => k.startsWith(key + '|') && avatarCache.delete(k));
        if (JSON.stringify(prev) !== JSON.stringify(next)) {
          store.contacts[key] = next;
          changed = true;
        }
      }
      if (changed) {
        saveSoon();
        bump();
      }
    }
    sock.ev.on('contacts.upsert', upsertContacts);
    sock.ev.on('contacts.update', upsertContacts);

    sock.ev.on('chats.upsert', (list) => {
      for (const c of list) {
        if (skipJid(c.id) || c.id === 'status@broadcast') continue;
        const chat = chatFor(canon(c.id));
        if (c.name) chat.name = c.name;
      }
      saveSoon();
    });
    sock.ev.on('chats.update', (list) => {
      let changed = false;
      for (const c of list) {
        const chat = store.chats[canon(c.id)];
        if (!chat) continue;
        if (c.name) chat.name = c.name;
        if (typeof c.unreadCount === 'number') chat.unread = Math.max(0, c.unreadCount);
        chat.ver++;
        changed = true;
      }
      if (changed) {
        saveSoon();
        bump();
      }
    });

    sock.ev.on('messages.upsert', ({ messages, type }) => {
      let changed = false;
      for (const m of messages) changed = addMessage(m, { live: type === 'notify' }) || changed;
      if (changed) {
        saveSoon();
        bump();
      }
    });

    sock.ev.on('messages.update', (updates) => {
      let changed = false;
      for (const { key, update } of updates) {
        const e = findEntry(key.remoteJid, key.id);
        if (!e) continue;
        let c = false;
        if (typeof update.status === 'number' && update.status > (e.ack || 0)) {
          e.ack = update.status;
          c = true;
        }
        if (update.starred != null) {
          e.starred = !!update.starred;
          c = true;
        }
        if (update.message?.protocolMessage?.type === 0 || update.messageStubType === 1) {
          e.revoked = true;
          c = true;
        }
        if (c) {
          touchChat(canon(key.remoteJid));
          changed = true;
        }
      }
      if (changed) {
        saveSoon();
        bump();
      }
    });

    sock.ev.on('messages.reaction', (list) => {
      let changed = false;
      for (const { key, reaction } of list) {
        const who = reaction.key?.fromMe ? myJid() : reaction.key?.participant || reaction.key?.remoteJid;
        changed = applyReaction({ key, text: reaction.text }, who) || changed;
        if (!reaction.key?.fromMe && reaction.text && key?.fromMe) {
          if (key.remoteJid === 'status@broadcast') {
            notify('slike', canon(who), canon(who), reaction.text, 'reaccionó a tu estado');
          } else {
            const e = findEntry(key.remoteJid, key.id);
            notify('react', canon(key.remoteJid), canon(who), reaction.text, e ? summary(entryInfo(e)) : 'tu mensaje');
          }
        }
      }
      if (changed) {
        saveSoon();
        bump();
      }
    });

    sock.ev.on('presence.update', async ({ id, presences }) => {
      let jid = canon(id);
      // la presencia suele llegar con el @lid aunque el chat esté guardado con el número
      if (isLid(jid) && !store.chats[jid]) {
        try {
          const pn = await sock.signalRepository?.lidMapping?.getPNForLID(jid);
          if (pn) { rememberLid(jid, jidNormalizedUser(pn)); jid = canon(jid); }
        } catch {}
      }
      for (const [rawWho, p] of Object.entries(presences || {})) {
        const who = canon(rawWho);
        const s = p.lastKnownPresence;
        presence[jid] = { state: s, lastSeen: p.lastSeen || presence[jid]?.lastSeen, who };
        setTyping(jid, who, s === 'composing' || s === 'recording' ? s : null);
      }
      if (store.chats[jid]) touchChat(jid);
      bump();
    });
  } catch (e) {
    log('error iniciando WhatsApp:', e?.message || e);
    setTimeout(startWA, Math.min(30000, 2000 * 2 ** retries++));
  } finally {
    starting = false;
  }
}

// ---------- cache de imagenes ----------
const mediaCache = new Map();
function cacheGet(k) {
  const v = mediaCache.get(k);
  if (v) {
    mediaCache.delete(k);
    mediaCache.set(k, v);
  }
  return v;
}
function cachePut(k, v) {
  mediaCache.set(k, v);
  while (mediaCache.size > 300) mediaCache.delete(mediaCache.keys().next().value);
}

async function downloadEntry(entry) {
  if (entry.phone) {
    const stream = await downloadContentFromMessage(entry.raw.message.stickerMessage, 'sticker');
    const chunks = [];
    for await (const c of stream) chunks.push(c);
    return Buffer.concat(chunks);
  }
  return downloadMediaMessage(entry.raw, 'buffer', {}, { logger, reuploadRequest: sock.updateMediaMessage });
}

// Sticker animado → tira vertical de cuadros (la Vita deduce cuadros = alto/ancho, 10 fps)
const notAnimated = new Set();
async function stickerStrip(entry, size) {
  const key = `${entry.id}:strip:${size}`;
  const hit = cacheGet(key);
  if (hit) return hit;
  if (notAnimated.has(entry.id)) return null;
  const msg = unwrap(entry.raw.message)?.stickerMessage;
  if (msg && msg.isAnimated === false) { notAnimated.add(entry.id); return null; }
  const buf = await downloadEntry(entry);
  const img = sharp(buf, { animated: true });
  const meta = await img.metadata();
  const pages = meta.pages || 1;
  if (pages <= 1) { notAnimated.add(entry.id); return null; }
  const ph = meta.pageHeight || meta.height;
  const delays = meta.delay?.length ? meta.delay : Array(pages).fill(100);
  const { data, info } = await img.ensureAlpha().raw().toBuffer({ resolveWithObject: true });
  const starts = []; let t = 0;
  for (let i = 0; i < pages; i++) { starts.push(t); t += Math.max(20, delays[i] || 100); }
  const n = Math.max(1, Math.min(Math.floor(4000 / size), 30, Math.round(t / 100)));
  const fb = info.width * ph * 4, frames = [];
  for (let k = 0; k < n; k++) {
    const at = (k * t) / n;
    let idx = 0; while (idx + 1 < pages && starts[idx + 1] <= at) idx++;
    frames.push(await sharp(data.subarray(idx * fb, (idx + 1) * fb), { raw: { width: info.width, height: ph, channels: 4 } })
      .resize(size, size, { fit: 'contain', background: { r: 0, g: 0, b: 0, alpha: 0 } }).raw().toBuffer());
  }
  const png = await sharp(Buffer.concat(frames), { raw: { width: size, height: size * n, channels: 4 } }).png({ compressionLevel: 9 }).toBuffer();
  cachePut(key, png);
  return png;
}

async function mediaPng(entry, max) {
  const key = `${entry.id}:${max}`;
  const hit = cacheGet(key);
  if (hit) return hit;
  const msg = unwrap(entry.raw.message);
  let buf;
  if (msg.videoMessage) {
    buf = msg.videoMessage.jpegThumbnail; // la Vita solo muestra un cuadro fijo
  } else {
    try {
      buf = await downloadEntry(entry);
    } catch {
      buf = msg.imageMessage?.jpegThumbnail || msg.stickerMessage?.pngThumbnail;
    }
  }
  if (!buf?.length) return null;
  const png = await sharp(Buffer.from(buf), { animated: false })
    .rotate()
    .resize({ width: max, height: max, fit: 'inside', withoutEnlargement: !!msg.stickerMessage })
    .png({ compressionLevel: 6 })
    .toBuffer();
  cachePut(key, png);
  return png;
}

async function emojiPng(name, size) {
  if (!/^[0-9a-f-]{2,64}$/.test(name)) return null;
  const key = `emoji:${name}:${size}`;
  const hit = cacheGet(key);
  if (hit) return hit;
  const file = path.join(TWEMOJI_DIR, name + '.svg');
  if (!fs.existsSync(file)) return null;
  const png = await sharp(fs.readFileSync(file), { density: 300 }).resize(size, size).png().toBuffer();
  cachePut(key, png);
  return png;
}

// Fotos de perfil: ya recortadas en circulo para que la Vita solo las pegue.
const avatarCache = new Map(); // "jid|size" -> { png|null, at }
let avatarBusy = 0;
const avatarQueue = [];
function avatarSlot() {
  if (avatarBusy < 2) {
    avatarBusy++;
    return Promise.resolve();
  }
  return new Promise((r) => avatarQueue.push(r));
}
function avatarDone() {
  const next = avatarQueue.shift();
  if (next) next();
  else avatarBusy--;
}
async function avatarPng(jid, size) {
  const k = `${jid}|${size}`;
  const hit = avatarCache.get(k);
  if (hit && Date.now() - hit.at < (hit.png ? 24 : 6) * 3600 * 1000) return hit.png;
  if (st.state !== 'open') return null;
  await avatarSlot();
  try {
    const url = await sock.profilePictureUrl(jid, size > 96 ? 'image' : 'preview', 8000).catch(() => null);
    let png = null;
    if (url) {
      const res = await fetch(url);
      if (res.ok) {
        const img = Buffer.from(await res.arrayBuffer());
        const mask = Buffer.from(`<svg width="${size}" height="${size}"><circle cx="${size / 2}" cy="${size / 2}" r="${size / 2}" fill="#fff"/></svg>`);
        png = await sharp(img).resize(size, size, { fit: 'cover' }).composite([{ input: mask, blend: 'dest-in' }]).png().toBuffer();
      }
    }
    avatarCache.set(k, { png, at: Date.now() });
    return png;
  } finally {
    avatarDone();
  }
}

// ---------- vistas para la Vita ----------
function chatLast(jid) {
  const list = store.msgs[jid] || [];
  const e = list[list.length - 1];
  if (!e) return '';
  const s = summary(entryInfo(e));
  if (e.fromMe) return `Tú: ${s}`;
  if (isGroup(jid)) return `${contactOf(e.sender).name || e.push || nameFor(e.sender)}: ${s}`;
  return s;
}
function lastKind(jid) {
  const list = store.msgs[jid] || [];
  const e = list[list.length - 1];
  return e ? entryInfo(e).kind : '';
}
function reactionsOf(e) {
  const counts = {};
  for (const r of Object.values(e.reactions || {})) counts[r] = (counts[r] || 0) + 1;
  return Object.entries(counts)
    .map(([emo, n]) => (n > 1 ? `${emo}${n}` : emo))
    .join(' ');
}
function quotedOf(d) {
  const ctx = d?.ctx;
  if (!ctx?.quotedMessage) return ['', ''];
  return [nameFor(ctx.participant) || '', summary(describeMessage(ctx.quotedMessage))];
}
function senderLabel(jid, e) {
  if (e.fromMe) return 'Tú';
  if (!isGroup(jid)) return chatName(jid);
  const c = contactOf(e.sender);
  return c.name || (e.push ? '~ ' + e.push : nameFor(e.sender));
}
function messageRows(jid, limit) {
  return (store.msgs[jid] || [])
    .slice(-limit)
    .map((e) => {
      const d = entryInfo(e);
      const [qn, qt] = quotedOf(d);
      return row(
        e.id, e.fromMe ? 1 : 0, senderLabel(jid, e), colorIdx(e.sender), hhmm(e.ts), dayLabel(e.ts),
        d.kind, d.text, qn, qt, reactionsOf(e), e.ack || 0, e.starred ? 1 : 0,
        d.ctx?.isForwarded ? 1 : 0, dur(d.dur), canon(e.sender) || '',
      );
    })
    .join('');
}
function statusEntries() {
  const rows = [];
  for (const [poster, list] of Object.entries(store.status)) {
    if (!list.length) continue;
    const last = list[list.length - 1];
    rows.push({ poster, name: poster === 'me' ? 'Mi estado' : chatName(poster), count: list.length, unseen: list.filter((x) => !x.seen).length, ts: last.ts });
  }
  return rows.sort((a, b) => (a.poster === 'me' ? -1 : b.poster === 'me' ? 1 : (b.unseen > 0) - (a.unseen > 0) || b.ts - a.ts));
}
function argbHex(n) {
  if (n == null) return '';
  return ((Number(n) >>> 0) & 0xffffff).toString(16).padStart(6, '0');
}
const fold = (s) => String(s || '').normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();

// ---------- rutas ----------
// Cada ruta recibe { q, body, signal } y devuelve texto, o { code, type, data }.
const text = (s) => ({ code: 200, type: 'text/plain; charset=utf-8', data: s });
const png = (b) => ({ code: 200, type: 'image/png', data: b });
const fail = (code, msg) => ({ code, type: 'text/plain; charset=utf-8', data: msg + '\n' });
const notOpen = () => (st.state === 'open' ? null : fail(409, 'no conectado'));

const routes = {
  'GET /v1/status': async ({ q, signal }) => {
    const since = Number(q.get('since') || 0);
    const wait = Math.min(LONG_POLL_MAX_S, Number(q.get('wait') || 0));
    if (since && since === st.seq && wait > 0) {
      await new Promise((resolve) => {
        const t = setTimeout(resolve, wait * 1000);
        const done = () => {
          clearTimeout(t);
          resolve();
        };
        waiters.push(done);
        signal?.addEventListener('abort', done);
      });
    }
    const unread = Object.values(store.chats).reduce((n, c) => n + (c.unread || 0), 0);
    lastVitaSeen = Date.now();
    return text(row(st.seq, st.state, st.me?.name || '', formatNumber(st.me?.jid) || st.me?.number || '', unread, st.statusVer, VERSION, st.me?.jid || '', notifSeq, st.state === 'qr' ? (st.pairCode || '') : ''));
  },

  'GET /v1/qr': () => {
    if (!st.qr) return fail(404, 'sin qr');
    const left = Math.max(0, 20 - Math.floor((Date.now() - st.qr.at) / 1000));
    return text(row(st.qr.size, st.seq, left) + st.qr.rows.join('\n') + '\n');
  },

  // jid  nombre  no_leidos  hora  ultimo  ver  es_grupo  tipo_ultimo  color  escribiendo
  'GET /v1/chats': ({ q }) => {
    const limit = Math.min(200, Number(q.get('limit') || 60));
    const list = Object.values(store.chats)
      .filter((c) => c.ts > 0 && !skipJid(c.jid) && c.jid !== 'status@broadcast' && !isMe(c.jid))
      .sort((a, b) => b.ts - a.ts)
      .slice(0, limit);
    for (const c of list) if (isGroup(c.jid) && !c.name) ensureGroupMeta(c.jid);
    return text(list.map((c) => row(c.jid, chatName(c.jid), c.unread || 0, listTime(c.ts), chatLast(c.jid), c.ver || 0, isGroup(c.jid) ? 1 : 0, lastKind(c.jid), colorIdx(c.jid), typingLabel(c.jid))).join(''));
  },

  // 1a linea: nombre  subtitulo  es_grupo  ver  numero
  // luego:    id  mio  remitente  color  hora  dia  tipo  texto  cita_nombre  cita_texto  reacciones  ack  destacado  reenviado  duracion  remitente_jid
  'GET /v1/chat': ({ q }) => {
    const jid = canon(q.get('jid'));
    const limit = Math.min(MAX_MSGS_PER_CHAT, Number(q.get('limit') || 60));
    if (isGroup(jid)) ensureGroupMeta(jid);
    else if (st.state === 'open' && jid) sock.presenceSubscribe(jid).catch(() => {});
    const c = store.chats[jid];
    return text(row(chatName(jid), subtitleFor(jid), isGroup(jid) ? 1 : 0, c?.ver || 0, isGroup(jid) ? '' : formatNumber(jid)) + messageRows(jid, limit));
  },

  // Info de contacto o de grupo
  //  persona: I  nombre  numero  info  info_fecha  nombre_agenda  nombre_perfil  subtitulo
  //           G  jid  nombre_grupo  participantes        (grupos en comun)
  //  grupo:   I  nombre  ''  descripcion  ''  ''  ''  subtitulo  participantes
  //           P  jid  nombre  admin  color
  'GET /v1/contact': async ({ q }) => {
    const jid = canon(q.get('jid'));
    if (!jid) return fail(400, 'falta jid');
    if (isGroup(jid)) {
      const g = (await ensureGroupMeta(jid, true)) || {};
      const parts = g.participants || [];
      let outRows = row('I', chatName(jid), '', g.desc || '', '', '', '', subtitleFor(jid), parts.length);
      const sorted = [...parts].sort((a, b) => (b.admin ? 1 : 0) - (a.admin ? 1 : 0) || nameFor(a.id).localeCompare(nameFor(b.id)));
      for (const p of sorted.slice(0, 200)) outRows += row('P', p.id, isMe(p.id) ? 'Tú' : nameFor(p.id), p.admin ? 1 : 0, colorIdx(p.id));
      return text(outRows);
    }
    let about = '', aboutAt = '';
    if (st.state === 'open') {
      try {
        const res = await sock.fetchStatus(jid);
        const s = res?.[0]?.status;
        if (s?.status) {
          about = s.status;
          if (s.setAt) aboutAt = `Actualizado ${agoLabel(new Date(s.setAt).getTime() / 1000)}`;
        }
      } catch {}
    }
    const c = contactOf(jid);
    let outRows = row('I', chatName(jid), formatNumber(jid), about, aboutAt, c.name || '', c.notify ? '~ ' + c.notify : '', subtitleFor(jid));
    const groups = (await participatingGroups()).filter((g) => g.members.has(jid));
    for (const g of groups.slice(0, 30)) outRows += row('G', g.id, g.subject || 'Grupo', g.members.size);
    return text(outRows);
  },

  // Contactos para "chat nuevo":  jid  nombre  numero  color  nombre_de_perfil
  'GET /v1/contacts': ({ q }) => {
    const term = fold(q.get('q'));
    const seen = new Set();
    const list = [];
    const consider = (jid) => {
      jid = canon(jid);
      if (!isPn(jid) || seen.has(jid) || isMe(jid)) return;
      seen.add(jid);
      const c = contactOf(jid);
      const name = c.name || c.verifiedName || store.chats[jid]?.name;
      if (!name && !store.chats[jid]) return;
      const label = name || nameFor(jid);
      const hay = fold(`${label} ${c.notify || ''}`);
      if (term && !hay.includes(term) && !jid.includes(term.replace(/\D/g, '') || '§')) return;
      list.push({ jid, label, saved: !!name, push: c.notify || '' });
    };
    Object.keys(store.contacts).forEach(consider);
    Object.keys(store.chats).forEach(consider);
    list.sort((a, b) => b.saved - a.saved || a.label.localeCompare(b.label, 'es'));
    return text(list.slice(0, 150).map((x) => row(x.jid, x.label, formatNumber(x.jid), colorIdx(x.jid), x.push ? '~ ' + x.push : '')).join(''));
  },

  // Revisa si un numero tiene WhatsApp:  jid  nombre  numero
  'GET /v1/check': async ({ q }) => {
    const n = notOpen();
    if (n) return n;
    let digits = String(q.get('number') || '').replace(/\D/g, '');
    if (digits.length < 8) return fail(400, 'número muy corto');
    if (digits.length === 8 && st.me?.number) {
      // sin codigo de pais: se asume el de tu propio numero
      const mine = parsePhoneNumberFromString('+' + st.me.number);
      if (mine?.countryCallingCode) digits = mine.countryCallingCode + digits;
    }
    const res = await sock.onWhatsApp(digits).catch(() => null);
    const hit = res?.find((r) => r.exists);
    if (!hit) return fail(404, 'ese número no tiene WhatsApp');
    const jid = canon(hit.jid);
    const chat = chatFor(jid);
    if (!chat.ts) chat.ts = Math.floor(Date.now() / 1000);
    saveSoon();
    bump();
    return text(row(jid, chatName(jid), formatNumber(jid)));
  },

  // poster  nombre  total  sin_ver  hace  color
  'GET /v1/statuses': () => text(statusEntries().map((s) => row(s.poster, s.name, s.count, s.unseen, agoLabel(s.ts), colorIdx(s.poster))).join('')),

  // id  tipo  texto  color_fondo  hace  visto  duracion
  'GET /v1/status_items': ({ q }) => {
    const list = store.status[q.get('poster')] || [];
    return text(list.map((e) => {
      const d = describeMessage(e.raw.message) || { kind: 'other', text: '' };
      return row(e.id, d.kind, d.text, argbHex(d.bg), agoLabel(e.ts), e.seen ? 1 : 0, dur(d.dur));
    }).join(''));
  },

  'GET /v1/media': async ({ q }) => {
    const n = notOpen();
    if (n) return n;
    const max = Math.max(32, Math.min(960, Number(q.get('max') || 280)));
    const poster = q.get('poster');
    const e = poster ? (store.status[poster] || []).find((x) => x.id === q.get('id')) : findEntry(q.get('jid'), q.get('id'));
    if (!e) return fail(404, 'no existe');
    if (q.get('anim') === '1' && unwrap(e.raw.message)?.stickerMessage) {
      const strip = await stickerStrip(e, Math.min(160, max)).catch(() => null);
      if (strip) return png(strip);
    }
    const p = await mediaPng(e, max).catch(() => null);
    return p ? png(p) : fail(404, 'sin media');
  },

  'GET /v1/avatar': async ({ q }) => {
    const jid = canon(q.get('jid'));
    const size = Math.max(32, Math.min(320, Number(q.get('s') || 96)));
    const p = jid ? await avatarPng(jid, size).catch(() => null) : null;
    return p ? png(p) : fail(404, 'sin foto');
  },

  // Nota de voz para la Vita: mu-law 16 kHz mono
  'GET /v1/audio': async ({ q }) => {
    const n = notOpen();
    if (n) return n;
    const e = findEntry(q.get('jid'), q.get('id'));
    if (!e) return fail(404, 'no existe');
    const key = `audio:${e.id}`;
    let buf = cacheGet(key);
    if (!buf) {
      const ogg = await downloadEntry(e);
      buf = (await oggToMulaw(ogg)).mulaw;
      cachePut(key, buf);
    }
    return { code: 200, type: 'audio/basic', data: buf };
  },

  'GET /v1/emoji': async ({ q }) => {
    const size = Math.max(16, Math.min(96, Number(q.get('s') || 32)));
    const p = await emojiPng(String(q.get('n') || ''), size).catch(() => null);
    return p ? png(p) : fail(404, 'no existe');
  },

  // pestaña "recientes" = los que mandaste desde la Vita + los recientes de tu teléfono
  'GET /v1/stickers': ({ q }) => {
    const tab = { fav: 'fav', received: 'received' }[q.get('tab')] || 'recent';
    let list = store.stickers[tab] || [];
    if (tab === 'recent') {
      const seen = new Set(list.map((s) => s.id));
      list = [...list, ...(store.stickers.phone || []).filter((s) => !seen.has(s.id)).map((s) => ({ jid: '@phone', id: s.id }))];
    }
    return text(list.slice(0, 64).map((s) => row(s.jid, s.id, s.anim ? 1 : 0)).join(''));
  },

  'POST /v1/send': async ({ q, body }) => {
    const n = notOpen();
    if (n) return n;
    const jid = canon(q.get('jid'));
    const msg = body.toString('utf8').trim();
    if (!jid || !msg) return fail(400, 'falta jid o texto');
    const quoted = q.get('reply') ? findEntry(jid, q.get('reply'))?.raw : undefined;
    const sent = await sock.sendMessage(jid, { text: msg }, quoted ? { quoted } : {});
    if (sent) addMessage(sent, { live: false });
    saveSoon();
    bump();
    return text(row(sent?.key?.id || ''));
  },

  // Foto desde la Vita: archivo (jpg/png/bmp) o cuadro crudo de la camara (fmt=i420&w=&h=)
  'POST /v1/image': async ({ q, body }) => {
    const n = notOpen();
    if (n) return n;
    const jid = canon(q.get('jid'));
    if (!jid || !body.length) return fail(400, 'falta jid o imagen');
    let img;
    if (q.get('fmt') === 'i420') {
      const w = Number(q.get('w')), h = Number(q.get('h'));
      if (!w || !h || body.length < (w * h * 3) / 2) return fail(400, 'cuadro incompleto');
      const rgb = Buffer.alloc(w * h * 3);
      const Y = body, U = w * h, V = U + (w * h) / 4;
      for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
          const yy = Y[y * w + x] - 16;
          const u = body[U + (y >> 1) * (w >> 1) + (x >> 1)] - 128;
          const v = body[V + (y >> 1) * (w >> 1) + (x >> 1)] - 128;
          const i = (y * w + x) * 3;
          rgb[i] = Math.max(0, Math.min(255, (298 * yy + 409 * v + 128) >> 8));
          rgb[i + 1] = Math.max(0, Math.min(255, (298 * yy - 100 * u - 208 * v + 128) >> 8));
          rgb[i + 2] = Math.max(0, Math.min(255, (298 * yy + 516 * u + 128) >> 8));
        }
      }
      img = sharp(rgb, { raw: { width: w, height: h, channels: 3 } });
      if (q.get('mirror') === '1') img = img.flop();
    } else {
      img = sharp(body).rotate();
    }
    const jpeg = await img.resize({ width: 1600, height: 1600, fit: 'inside', withoutEnlargement: true }).jpeg({ quality: 85 }).toBuffer();
    const quoted = q.get('reply') ? findEntry(jid, q.get('reply'))?.raw : undefined;
    const caption = (q.get('caption') || '').trim();
    const sent = await sock.sendMessage(jid, { image: jpeg, caption: caption || undefined }, quoted ? { quoted } : {});
    if (sent) addMessage(sent, { live: false });
    saveSoon();
    bump();
    return text('ok\n');
  },

  // Nota de voz grabada en la Vita (mu-law 16 kHz mono)
  'POST /v1/voice': async ({ q, body }) => {
    const n = notOpen();
    if (n) return n;
    const jid = canon(q.get('jid'));
    if (!jid || body.length < 1600) return fail(400, 'audio muy corto');
    const { ogg, seconds, waveform } = mulawToOgg(body);
    const quoted = q.get('reply') ? findEntry(jid, q.get('reply'))?.raw : undefined;
    const sent = await sock.sendMessage(jid, { audio: ogg, mimetype: 'audio/ogg; codecs=opus', ptt: true, seconds, waveform }, quoted ? { quoted } : {});
    if (sent) addMessage(sent, { live: false });
    saveSoon();
    bump();
    return text('ok\n');
  },

  'POST /v1/sticker': async ({ q }) => {
    const n = notOpen();
    if (n) return n;
    const jid = canon(q.get('jid'));
    const src = findEntry(q.get('src'), q.get('id'));
    if (!jid || !src) return fail(404, 'sticker no encontrado');
    const buf = await downloadEntry(src);
    const quoted = q.get('reply') ? findEntry(jid, q.get('reply'))?.raw : undefined;
    const sent = await sock.sendMessage(jid, { sticker: buf }, quoted ? { quoted } : {});
    if (sent) addMessage(sent, { live: false });
    saveSoon();
    bump();
    return text('ok\n');
  },

  'POST /v1/sticker_fav': ({ q }) => {
    const jid = q.get('jid');
    const id = q.get('id');
    const fav = store.stickers.fav;
    const i = fav.findIndex((s) => s.id === id);
    if (q.get('on') === '0') {
      if (i >= 0) fav.splice(i, 1);
    } else if (i < 0) fav.unshift({ jid, id });
    saveSoon();
    return text('ok\n');
  },

  'POST /v1/react': async ({ q }) => {
    const n = notOpen();
    if (n) return n;
    const jid = canon(q.get('jid'));
    const e = findEntry(jid, q.get('id'));
    if (!e) return fail(404, 'no existe');
    const emo = String(q.get('e') || '')
      .split('-')
      .filter((h) => /^[0-9a-f]{2,6}$/i.test(h))
      .map((h) => String.fromCodePoint(parseInt(h, 16)))
      .join('');
    await sock.sendMessage(jid, { react: { text: emo, key: e.raw.key } });
    applyReaction({ key: e.raw.key, text: emo }, myJid());
    saveSoon();
    bump();
    return text('ok\n');
  },

  'POST /v1/forward': async ({ q }) => {
    const n = notOpen();
    if (n) return n;
    const e = findEntry(q.get('jid'), q.get('id'));
    const to = String(q.get('to') || '').split(',').filter(Boolean).slice(0, 5).map(canon);
    if (!e || !to.length) return fail(400, 'falta mensaje o destino');
    for (const t of to) {
      const sent = await sock.sendMessage(t, { forward: e.raw });
      if (sent) addMessage(sent, { live: false });
    }
    saveSoon();
    bump();
    return text('ok\n');
  },

  'POST /v1/delete': async ({ q }) => {
    const n = notOpen();
    if (n) return n;
    const jid = canon(q.get('jid'));
    const list = store.msgs[jid] || [];
    const i = list.findIndex((x) => x.id === q.get('id'));
    if (i < 0) return fail(404, 'no existe');
    const e = list[i];
    await sock.chatModify({ deleteForMe: { deleteMedia: false, key: e.raw.key, timestamp: e.ts } }, jid).catch(() => {});
    list.splice(i, 1);
    touchChat(jid);
    saveSoon();
    bump();
    return text('ok\n');
  },

  'POST /v1/star': async ({ q }) => {
    const n = notOpen();
    if (n) return n;
    const jid = canon(q.get('jid'));
    const e = findEntry(jid, q.get('id'));
    if (!e) return fail(404, 'no existe');
    const on = q.get('on') !== '0';
    await sock.chatModify({ star: { messages: [{ id: e.id, fromMe: e.fromMe }], star: on } }, jid).catch(() => {});
    e.starred = on;
    touchChat(jid);
    saveSoon();
    bump();
    return text('ok\n');
  },

  'POST /v1/read': ({ q }) => {
    const jid = canon(q.get('jid'));
    const chat = store.chats[jid];
    if (chat) {
      const keys = (store.msgs[jid] || [])
        .filter((m) => !m.fromMe)
        .slice(-Math.max(1, chat.unread || 0))
        .map((m) => m.raw.key);
      chat.unread = 0;
      chat.ver++;
      if (st.state === 'open' && keys.length) sock.readMessages(keys).catch(() => {});
      saveSoon();
      bump();
    }
    return text('ok\n');
  },

  'POST /v1/status_seen': ({ q }) => {
    const e = (store.status[q.get('poster')] || []).find((x) => x.id === q.get('id'));
    if (e && !e.seen) {
      e.seen = true;
      if (st.state === 'open') sock.readMessages([e.raw.key]).catch(() => {});
      st.statusVer++;
      saveSoon();
      bump();
    }
    return text('ok\n');
  },

  'POST /v1/status_reply': async ({ q, body }) => {
    const n = notOpen();
    if (n) return n;
    const poster = q.get('poster');
    const e = (store.status[poster] || []).find((x) => x.id === q.get('id'));
    const msg = body.toString('utf8').trim();
    if (!e || !msg) return fail(400, 'falta estado o texto');
    const sent = await sock.sendMessage(poster, { text: msg }, { quoted: e.raw });
    if (sent) addMessage(sent, { live: false });
    saveSoon();
    bump();
    return text('ok\n');
  },

  'POST /v1/status_post': async ({ q, body }) => {
    const n = notOpen();
    if (n) return n;
    const msg = body.toString('utf8').trim();
    if (!msg) return fail(400, 'falta texto');
    const bg = /^[0-9a-f]{6}$/i.test(q.get('bg') || '') ? `#${q.get('bg')}` : '#1E6FD9';
    const audience = new Set(Object.keys(store.contacts).map(canon).filter(isPn));
    audience.add(myJid());
    await sock.sendMessage('status@broadcast', { text: msg }, { backgroundColor: bg, font: 0, statusJidList: [...audience] });
    st.statusVer++;
    saveSoon();
    bump();
    return text('ok\n');
  },

  // Vincular WhatsApp con NÚMERO (código de 8 caracteres) en vez de QR
  'POST /v1/wapair': async ({ q }) => {
    const num = String(q.get('n') || '').replace(/\D/g, '');
    if (num.length < 8 || num.length > 15) return fail(400, 'Número inválido: usa código de país, ej. 50212345678');
    if (st.state === 'open') return fail(409, 'WhatsApp ya está vinculado');
    if (st.state !== 'qr' || !sock) return fail(409, 'El bridge aún no está listo, intenta en unos segundos');
    const raw = await sock.requestPairingCode(num);
    st.pairCode = raw.length === 8 ? `${raw.slice(0, 4)}-${raw.slice(4)}` : raw;
    log('código de vinculación generado');
    bump();
    return text(row(st.pairCode));
  },

  // notificaciones nuevas:  seq  tipo  jid  titulo  emoji  texto  hora
  'GET /v1/notifs': ({ q }) => {
    const since = Number(q.get('since') || 0);
    return text(notifs.filter((n) => n.seq > since).map((n) => {
      const who = nameFor(n.who);
      const title = n.kind === 'msg' && isGroup(n.jid) ? `${who} @ ${chatName(n.jid)}` : (n.kind === 'msg' ? chatName(n.jid) : who);
      return row(n.seq, n.kind, n.jid, title, n.emoji, n.body, listTime(n.ts));
    }).join(''));
  },

  'POST /v1/logout': async () => {
    if (sock && st.state === 'open') await sock.logout().catch(() => {});
    return text('ok\n');
  },
};

async function dispatch(method, pathq, body, signal) {
  const url = new URL(pathq, 'http://x');
  const fn = routes[`${method} ${url.pathname}`];
  if (!fn) return fail(404, 'no existe');
  if (url.searchParams.has('_tz')) setTz(url.searchParams.get('_tz'));
  const r = await fn({ q: url.searchParams, body: body || Buffer.alloc(0), signal });
  return typeof r === 'string' ? text(r) : r;
}

// ---------- HTTP ----------
function readBody(req, limit) {
  return new Promise((resolve, reject) => {
    let size = 0;
    const parts = [];
    req.on('data', (c) => {
      size += c.length;
      if (size > limit) {
        reject(Object.assign(new Error('muy grande'), { code: 413 }));
        req.destroy();
      } else parts.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(parts)));
    req.on('error', reject);
  });
}

const isLocal = (req) => ['127.0.0.1', '::1', '::ffff:127.0.0.1'].includes(req.socket.remoteAddress);

async function statusPage() {
  const pairSvg = await QRCode.toString(PAIR_TEXT, { type: 'svg', errorCorrectionLevel: 'M', margin: 4 });
  const waSvg = st.qr ? await QRCode.toString(st.qr.raw, { type: 'svg', errorCorrectionLevel: 'L', margin: 2 }) : '';
  const estado = { open: `Conectado como ${st.me?.name || ''} (${formatNumber(st.me?.jid)})`, qr: 'Esperando que vincules WhatsApp', connecting: 'Conectando con WhatsApp…', starting: 'Iniciando…', logged_out: 'Sesión cerrada' }[st.state] || st.state;
  return `<!doctype html><html lang="es"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta http-equiv="refresh" content="15"><title>VitaWA bridge</title>
<style>body{margin:0;font-family:system-ui,sans-serif;background:#EEF2F6;color:#1E2A36}main{max-width:900px;margin:0 auto;padding:24px;display:flex;flex-wrap:wrap;gap:24px}
section{flex:1 1 360px;background:#fff;border:1px solid #D6DEE7;border-radius:16px;padding:20px}h1{font-size:22px;margin:0 0 6px}p{color:#5B6876;line-height:1.5}
.qr{background:#fff;border-radius:12px;max-width:340px}.qr svg{width:100%;height:auto;display:block;shape-rendering:crispEdges}
.pair{flex-basis:100%;text-align:center}.pair .qr{max-width:none;width:min(72vh,92vw);margin:8px auto}.big{font:600 18px ui-monospace,Consolas,monospace;word-break:break-all;background:#EEF2F6;padding:10px;border-radius:10px;display:inline-block;max-width:100%}code{background:#EEF2F6;padding:2px 6px;border-radius:6px;word-break:break-all}</style></head>
<body><main>
<section class="pair"><h1>Emparejar la Vita</h1><p>Abre VitaWA en la Vita y apunta la cámara a este código, a unos 30–40 cm, que se vea completo.<br>Tip: baja un poco el brillo de la pantalla si la cámara ve reflejos.</p><div class="qr">${pairSvg}</div>
<p>¿No lo lee? En la Vita pulsa □ y escribe:</p><div class="big">${PAIR_TEXT}</div>
<p>Dirección que verá la Vita: <code>${PUBLIC_URL}</code></p></section>
<section><h1>WhatsApp</h1><p>${estado}</p>${waSvg ? `<p>Escanéalo con el teléfono: WhatsApp → Dispositivos vinculados.</p><div class="qr">${waSvg}</div>` : ''}<p>Versión ${VERSION}</p></section>
</main></body></html>`;
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  const ac = new AbortController();
  res.on('close', () => ac.abort());
  try {
    if (url.pathname === '/health') {
      res.writeHead(200, { 'Content-Type': 'text/plain' });
      return res.end('ok\n');
    }

    // Canal cifrado de la Vita
    if (req.method === 'POST' && url.pathname === '/x') {
      let r;
      try {
        const raw = await readBody(req, MAX_UPLOAD + 1024);
        const { method, path: p, body } = openRequest(KEY, raw);
        r = await dispatch(method, p, body, ac.signal);
      } catch (e) {
        if (e.code === 401 || e.code === 400) {
          // sin la llave correcta no hay respuesta cifrada posible
          res.writeHead(e.code, { 'Content-Type': 'text/plain' });
          return res.end(e.code === 401 ? 'pareja\n' : 'formato\n');
        }
        r = e.code === 425 ? { code: 425, type: 'text/plain', data: String(Date.now()) } : fail(e.code || 500, e.message || 'error');
        if (!e.code) log('error:', e?.message || e);
      }
      if (res.destroyed) return;
      res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Cache-Control': 'no-store' });
      return res.end(sealResponse(KEY, r.code, r.type, r.data));
    }

    // Desde fuera del servidor (Render): la página con los QR se abre con /?k=ADMIN_TOKEN
    const tokenOk = ADMIN_TOKEN && ADMIN_TOKEN.length >= 12 && url.searchParams.get('k') === ADMIN_TOKEN;
    if (!isLocal(req) && tokenOk && req.method === 'GET' && url.pathname === '/') {
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
      return res.end(await statusPage());
    }
    // Solo desde esta misma PC: rutas sin cifrar para depurar
    if (!isLocal(req)) {
      res.writeHead(404, { 'Content-Type': 'text/plain' });
      return res.end('VitaWA bridge\n');
    }
    if (req.method === 'GET' && url.pathname === '/') {
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
      return res.end(await statusPage());
    }
    const body = req.method === 'POST' ? await readBody(req, MAX_UPLOAD) : Buffer.alloc(0);
    const r = await dispatch(req.method, req.url, body, ac.signal);
    if (res.destroyed) return;
    res.writeHead(r.code, { 'Content-Type': r.type, 'Cache-Control': 'no-store' });
    res.end(r.data);
  } catch (e) {
    log(`error en ${url.pathname}:`, e?.message || e);
    if (!res.headersSent) {
      res.writeHead(500, { 'Content-Type': 'text/plain' });
      res.end('error\n');
    }
  }
});

server.listen(PORT, '0.0.0.0', async () => {
  console.log('');
  console.log(`  VitaWA bridge ${VERSION} listo`);
  console.log(`  Direccion para la Vita: ${PUBLIC_URL}`);
  console.log(`  En esta PC abre http://localhost:${PORT} para ver el codigo en grande.`);
  console.log('');
  console.log('  Codigo de emparejamiento (escanealo con la camara de la Vita):');
  console.log(await QRCode.toString(PAIR_TEXT, { type: 'terminal', small: true, errorCorrectionLevel: 'M' }));
  console.log(`  Si no puedes escanearlo, en la Vita elige "Escribir el codigo" y teclea:`);
  console.log(`    ${PAIR_TEXT}`);
  console.log('');
  startWA();
  if (process.env.OPEN_BROWSER === '1') {
    const { exec } = await import('node:child_process');
    exec(process.platform === 'win32' ? `start "" http://localhost:${PORT}` : `xdg-open http://localhost:${PORT}`);
  }
});

for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => {
    log('cerrando...');
    saveNow();
    process.exit(0);
  });
}
