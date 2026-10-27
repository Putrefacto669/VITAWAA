// Canal cifrado entre la Vita y el bridge.
//
// Al emparejar, la Vita escanea un QR con:   VWA1 <url del bridge> <llave>
// Desde ahi cada peticion va en un solo POST /x:
//
//   cuerpo = nonce(24) | cifrado | mac(16)        XChaCha20-Poly1305, AD = "VWA1"
//   texto  = "METODO RUTA\n<hora unix ms>\n" + cuerpo de la peticion
//
// y la respuesta igual:   texto = "<codigo>\n<tipo>\n" + cuerpo
//
// Sin la llave nadie puede leer ni mandar nada, aunque el bridge este en internet
// sin HTTPS (la Vita no confia en los certificados modernos).

import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { xchacha20poly1305 } from '@noble/ciphers/chacha.js';

const AD = Buffer.from('VWA1');
const MAX_SKEW_MS = 10 * 60 * 1000;

export function loadKey(dataDir, reset = false) {
  const file = path.join(dataDir, 'pair.json');
  if (!reset && fs.existsSync(file)) {
    try {
      const j = JSON.parse(fs.readFileSync(file, 'utf8'));
      const key = Buffer.from(j.key, 'base64url');
      if (key.length === 32) return key;
    } catch {}
  }
  const key = crypto.randomBytes(32);
  fs.writeFileSync(file, JSON.stringify({ key: key.toString('base64url'), created: new Date().toISOString() }), { mode: 0o600 });
  return key;
}

export function pairingText(url, key) {
  // sin "http://" para que el QR salga con cuadros mas grandes (la Vita lo agrega)
  return `VWA1 ${String(url).replace(/^http:\/\//, '')} ${key.toString('base64url')}`;
}

const seen = new Map(); // nonce hex -> momento (contra repeticiones)
setInterval(() => {
  const limit = Date.now() - 2 * MAX_SKEW_MS;
  for (const [k, t] of seen) if (t < limit) seen.delete(k);
}, 60 * 1000).unref();

function seal(key, plain) {
  const nonce = crypto.randomBytes(24);
  const ct = xchacha20poly1305(key, nonce, AD).encrypt(plain);
  return Buffer.concat([nonce, Buffer.from(ct)]);
}

export function sealResponse(key, code, type, body) {
  const head = Buffer.from(`${code}\n${type}\n`);
  const data = Buffer.isBuffer(body) ? body : Buffer.from(String(body ?? ''), 'utf8');
  return seal(key, Buffer.concat([head, data]));
}

// Devuelve { method, path, body } o lanza un error con .code
export function openRequest(key, buf) {
  if (buf.length < 24 + 16) throw Object.assign(new Error('corto'), { code: 400 });
  const nonce = buf.subarray(0, 24);
  let plain;
  try {
    plain = Buffer.from(xchacha20poly1305(key, nonce, AD).decrypt(buf.subarray(24)));
  } catch {
    throw Object.assign(new Error('llave incorrecta'), { code: 401 });
  }
  const nl1 = plain.indexOf(10);
  const nl2 = plain.indexOf(10, nl1 + 1);
  if (nl1 < 0 || nl2 < 0) throw Object.assign(new Error('formato'), { code: 400 });
  const [method, ...rest] = plain.subarray(0, nl1).toString('utf8').split(' ');
  const pathq = rest.join(' ');
  const ts = Number(plain.subarray(nl1 + 1, nl2).toString('utf8'));
  if (!Number.isFinite(ts) || Math.abs(Date.now() - ts) > MAX_SKEW_MS) {
    // la Vita usa esta hora para corregir su reloj
    throw Object.assign(new Error('reloj'), { code: 425, sealed: true });
  }
  const id = nonce.toString('hex');
  if (seen.has(id)) throw Object.assign(new Error('repetida'), { code: 409, sealed: true });
  seen.set(id, ts);
  return { method, path: pathq, body: plain.subarray(nl2 + 1) };
}
