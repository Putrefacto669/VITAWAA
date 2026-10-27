// ─────────────────────────────────────────────────────────────────────────────
// Baileys decodifica el historial completo al vincular, pero solo nos pasa
// chats/contactos/mensajes. WhatsApp también manda ahí tus STICKERS RECIENTES
// (`recentStickers`) — los mismos del panel de stickers de tu teléfono.
// Este parche (idempotente) los deja pasar en el evento `messaging-history.set`.
// ─────────────────────────────────────────────────────────────────────────────
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const file = path.join(root, 'node_modules', 'baileys', 'lib', 'Utils', 'history.js');
if (!fs.existsSync(file)) { console.warn('[patch-baileys] no encontré history.js, me salto el parche'); process.exit(0); }
let src = fs.readFileSync(file, 'utf8');
if (src.includes('recentStickers: item.recentStickers')) { console.log('[patch-baileys] ya estaba aplicado'); process.exit(0); }
const needle = 'pastParticipants: item.pastParticipants,';
if (!src.includes(needle)) { console.warn('[patch-baileys] la versión de Baileys cambió; sin stickers recientes'); process.exit(0); }
src = src.replace(needle, needle + '\n        recentStickers: item.recentStickers,');
fs.writeFileSync(file, src);
console.log('[patch-baileys] listo: recentStickers expuesto');
