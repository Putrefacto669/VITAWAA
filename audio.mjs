// Audio para la Vita, sin ffmpeg (todo en JS/WebAssembly, funciona igual en Windows y en la nube).
//
//  WhatsApp -> Vita: Ogg/Opus -> PCM -> 16 kHz mono -> mu-law (8 bits por muestra)
//  Vita -> WhatsApp: mu-law 16 kHz -> PCM -> Opus -> Ogg  (nota de voz "ptt")
//
// mu-law es el formato de la telefonia: la mitad de bytes que PCM de 16 bits y
// se decodifica en la Vita con una tabla, sin gastar CPU.

import { OggOpusDecoder } from 'ogg-opus-decoder';
import OpusScript from 'opusscript';

export const RATE = 16000;

// ---------- mu-law ----------
function linearToMulaw(sample) {
  const BIAS = 0x84, CLIP = 32635;
  let sign = (sample >> 8) & 0x80;
  if (sign) sample = -sample;
  if (sample > CLIP) sample = CLIP;
  sample += BIAS;
  let exponent = 7;
  for (let mask = 0x4000; (sample & mask) === 0 && exponent > 0; exponent--, mask >>= 1);
  const mantissa = (sample >> (exponent + 3)) & 0x0f;
  return ~(sign | (exponent << 4) | mantissa) & 0xff;
}

const MULAW_TABLE = new Int16Array(256);
for (let i = 0; i < 256; i++) {
  const u = ~i & 0xff;
  const sign = u & 0x80, exponent = (u >> 4) & 0x07, mantissa = u & 0x0f;
  let s = ((mantissa << 3) + 0x84) << exponent;
  s -= 0x84;
  MULAW_TABLE[i] = sign ? -s : s;
}

// ---------- WhatsApp -> Vita ----------
let decoderPromise = null;
async function decoder() {
  if (!decoderPromise) {
    decoderPromise = (async () => {
      const d = new OggOpusDecoder();
      await d.ready;
      return d;
    })();
  }
  return decoderPromise;
}

export async function oggToMulaw(buf) {
  const d = await decoder();
  await d.reset();
  const { channelData, samplesDecoded, sampleRate } = await d.decodeFile(new Uint8Array(buf));
  const ch0 = channelData[0];
  const ch1 = channelData[1];
  const step = sampleRate / RATE;
  const n = Math.floor(samplesDecoded / step);
  const out = Buffer.alloc(n);
  for (let i = 0; i < n; i++) {
    // promedio simple de la ventana: filtro paso bajo barato antes de bajar la frecuencia
    const a = Math.floor(i * step), b = Math.min(samplesDecoded, Math.floor((i + 1) * step));
    let acc = 0;
    for (let k = a; k < b; k++) acc += ch1 ? (ch0[k] + ch1[k]) / 2 : ch0[k];
    const v = Math.max(-1, Math.min(1, acc / Math.max(1, b - a)));
    out[i] = linearToMulaw(Math.round(v * 32767));
  }
  return { mulaw: out, seconds: n / RATE };
}

// ---------- Vita -> WhatsApp ----------
const CRC_TABLE = new Uint32Array(256);
for (let i = 0; i < 256; i++) {
  let r = i << 24;
  for (let j = 0; j < 8; j++) r = r & 0x80000000 ? (r << 1) ^ 0x04c11db7 : r << 1;
  CRC_TABLE[i] = r >>> 0;
}
function oggCrc(buf) {
  let crc = 0;
  for (let i = 0; i < buf.length; i++) crc = ((crc << 8) ^ CRC_TABLE[((crc >>> 24) & 0xff) ^ buf[i]]) >>> 0;
  return crc >>> 0;
}

function oggPage({ packets, granule, serial, seq, bos, eos }) {
  const lacing = [];
  for (const p of packets) {
    let len = p.length;
    while (len >= 255) {
      lacing.push(255);
      len -= 255;
    }
    lacing.push(len);
  }
  const header = Buffer.alloc(27 + lacing.length);
  header.write('OggS', 0, 'ascii');
  header[4] = 0;
  header[5] = (bos ? 0x02 : 0) | (eos ? 0x04 : 0);
  header.writeBigInt64LE(BigInt(granule), 6);
  header.writeUInt32LE(serial, 14);
  header.writeUInt32LE(seq, 18);
  header.writeUInt32LE(0, 22);
  header[26] = lacing.length;
  for (let i = 0; i < lacing.length; i++) header[27 + i] = lacing[i];
  const page = Buffer.concat([header, ...packets]);
  page.writeUInt32LE(oggCrc(page), 22);
  return page;
}

export function mulawToOgg(mulaw) {
  const pcm = new Int16Array(mulaw.length);
  for (let i = 0; i < mulaw.length; i++) pcm[i] = MULAW_TABLE[mulaw[i]];

  const FRAME = RATE / 50; // 20 ms
  const PRESKIP = 312;     // en muestras de 48 kHz, lo usual para libopus
  const enc = new OpusScript(RATE, 1, OpusScript.Application.VOIP);
  try {
    enc.setBitrate?.(24000);
  } catch {}
  const packets = [];
  for (let off = 0; off < pcm.length; off += FRAME) {
    const frame = new Int16Array(FRAME);
    frame.set(pcm.subarray(off, Math.min(pcm.length, off + FRAME)));
    packets.push(Buffer.from(enc.encode(Buffer.from(frame.buffer), FRAME)));
  }
  enc.delete();

  const serial = (Math.random() * 0xffffffff) >>> 0;
  const head = Buffer.alloc(19);
  head.write('OpusHead', 0, 'ascii');
  head[8] = 1;
  head[9] = 1;
  head.writeUInt16LE(PRESKIP, 10);
  head.writeUInt32LE(RATE, 12);
  head.writeInt16LE(0, 16);
  head[18] = 0;
  const vendor = Buffer.from('VitaWA');
  const tags = Buffer.alloc(8 + 4 + vendor.length + 4);
  tags.write('OpusTags', 0, 'ascii');
  tags.writeUInt32LE(vendor.length, 8);
  vendor.copy(tags, 12);
  tags.writeUInt32LE(0, 12 + vendor.length);

  const pages = [oggPage({ packets: [head], granule: 0, serial, seq: 0, bos: true }), oggPage({ packets: [tags], granule: 0, serial, seq: 1 })];
  let seq = 2, done = 0;
  const PER_PAGE = 50;
  for (let i = 0; i < packets.length; i += PER_PAGE) {
    const chunk = packets.slice(i, i + PER_PAGE);
    done += chunk.length;
    const granule = PRESKIP + Math.min(done * FRAME, pcm.length) * (48000 / RATE);
    pages.push(oggPage({ packets: chunk, granule, serial, seq: seq++, eos: i + PER_PAGE >= packets.length }));
  }

  // forma de onda para la burbuja de WhatsApp: 64 valores de 0 a 100
  const waveform = new Uint8Array(64);
  const per = Math.max(1, Math.floor(pcm.length / 64));
  let peak = 1;
  const rms = [];
  for (let i = 0; i < 64; i++) {
    let acc = 0;
    for (let k = i * per; k < Math.min(pcm.length, (i + 1) * per); k++) acc += pcm[k] * pcm[k];
    const r = Math.sqrt(acc / per);
    rms.push(r);
    if (r > peak) peak = r;
  }
  rms.forEach((r, i) => (waveform[i] = Math.round((r / peak) * 100)));

  return { ogg: Buffer.concat(pages), seconds: Math.max(1, Math.round(pcm.length / RATE)), waveform };
}
