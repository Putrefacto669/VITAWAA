# VitaWA

Cliente de WhatsApp para PS Vita, con look del sistema y control con botones o pantalla táctil.

```
 Teléfono ──(QR)──▶ WhatsApp ◀──▶ bridge (Node + Baileys) ◀══ canal cifrado ══▶ PS Vita
                                   nube gratis / tu PC
```

La Vita no habla con WhatsApp directamente (no hay forma práctica de correr el protocolo de WhatsApp en la Vita). Habla con un **bridge** que vive en un servidor encendido 24/7. Lo instalas **una sola vez**; después, desde la Vita solo escaneas un QR y listo, estés donde estés.

> ⚠️ Un cliente no oficial va contra los términos de WhatsApp y puede llevar a un ban del número. Prueba primero con un número secundario.

## Novedades 3.6

- **Videos y GIFs** se reproducen en la Vita (× sobre el video): el bridge los convierte a 480×272 con ffmpeg y la Vita los decodifica por hardware. × pausa, ○ cierra. Máx. 3 min / 16 MB.
- **Reconecta sola al despertar** la Vita de la suspensión.
- **Silenciar, archivar y fijar** chats desde la info del contacto (L / R / △). Los fijados salen arriba con chincheta, los silenciados con altavoz tachado y **no dan avisos**. Filtro **Archivo** en Inicio.
- **Editar** (hasta 15 min) y **Eliminar para todos** en el menú □ de tus mensajes.
- **Buscar en el chat** con L (pulsa L otra vez para la coincidencia anterior).
- **Tamaño de letra** Normal / Grande en Ajustes.
- **Encuestas**: se ven las opciones con barras y votos (votar sigue siendo desde el teléfono).
- **Publicar estados con foto**: en la pestaña Estados, START = cámara, L = galería.
- **Panel de emojis** (△ → Emojis) con **Recientes** y categorías; también para reaccionar con cualquier emoji (botón **+** del menú de reacciones).
- **Avisos de la Vita**: cada notificación también llega al centro de notificaciones del sistema (se apaga en Ajustes).

## Novedades 3.5

- **Funciona en Render** (y cualquier hosting con https): la Vita ahora habla https con curl. Guía: **[GUIA-RENDER.md](GUIA-RENDER.md)**.
- **Vincular WhatsApp con tu número**: código de 8 caracteres en la Vita (□ alterna con el QR). El número se guarda.
- **Notificaciones**: banner que baja desde arriba y campanita 🔔 en Inicio con historial — mensajes nuevos, **reacciones a tus mensajes** y **reacciones a tus estados**.
- **"Escribiendo…" arreglado**: temporizador de 10 s por persona (ya no se queda pegado), funciona en grupos con varias personas, resuelve los contactos @lid y se ve también en la lista de chats con puntitos animados.
- **Stickers**: los **recientes de tu teléfono** aparecen en la pestaña Recientes (llegan al vincular) y los **animados se mueven** en el chat y en el panel.
- **Estética Aero**: fondo con degradado, burbujas y onda de luz; barras y tarjetas de vidrio con brillo; burbujas con sombra y reflejo; avatares tipo orbe. Tema **Aero claro** o **Aero noche** en Ajustes.

## Qué hace (fase 3)

- **Look nativo**: tema claro estilo sistema de la Vita, fuente M PLUS 1p, sonidos de interfaz (moverse, abrir, enviar, recibir) que se pueden apagar.
- **Pantalla táctil**: tocas chats, mensajes, pestañas y las ayudas de abajo (× Abrir, ○ Volver…). Las listas se arrastran con inercia.
- **Notas de voz**: se escuchan en la Vita (× sobre la nota) y se graban con el micrófono: **mantén R** (o el dedo sobre el micrófono) y suelta para enviar; ○ cancela. Máx. 2 minutos.
- **Fotos**: con la **cámara de la Vita** (delantera o trasera) o desde la **galería** (capturas y fotos de `ux0:picture`), con o sin texto.
- **Info de contacto**: foto de perfil grande, nombre guardado, nombre que se puso, número, info/"about" y grupos en común. En grupos: descripción y miembros con admins.
- **Nombres arreglados**: se usa el nombre guardado; si no hay, el número con formato (+505 8123 4567) y el nombre de perfil como `~Andy`.
- **Fotos de perfil** en la lista de chats, estados y conversaciones.
- **Nuevo chat** (START en Inicio): buscar contactos o escribir un número.
- Todo lo de la fase 2: estados, stickers, responder, reenviar, reacciones, destacar, eliminar para mí, emojis, "escribiendo…".

No hace: llamadas, votar encuestas, ni notificaciones con la app cerrada (el homebrew no corre en segundo plano).

## Cómo se usa

1. **Bridge** (una vez): sigue **[GUIA-SERVIDOR.md](GUIA-SERVIDOR.md)** para dejarlo en la nube gratis (Oracle Cloud), o usa la versión de Windows con doble clic para probar en casa.
2. **Vita**: instala `VitaWA.vpk` con VitaShell. Al abrirla la primera vez sale la cámara: apunta al **QR de emparejamiento** del bridge.
3. Sale el **QR de WhatsApp** en la Vita: escanéalo desde el teléfono en *Dispositivos vinculados*.

Listo. La Vita guarda el emparejamiento en `ux0:data/VitaWA/config.txt`.

### Controles

| Botón | Inicio | Chat |
|---|---|---|
| × | Abrir chat | Escuchar nota / ver foto / escribir |
| ○ | — | Volver (o quitar respuesta) |
| □ | Info del contacto | Acciones (reaccionar, responder, reenviar…) |
| △ | Ajustes | Adjuntar: cámara, galería, nota de voz, stickers |
| START | Nuevo chat | Escribir |
| SELECT | Chats ⇄ Estados | Info del contacto |
| R | Filtro siguiente | **Mantén para grabar** |
| L / R | Filtros | — |

## Seguridad

- Vita y bridge hablan por un **canal cifrado** (XChaCha20-Poly1305) con una clave de 32 bytes que viaja **solo** dentro del QR de emparejamiento. Sin esa clave nadie puede usar tu bridge, aunque conozca la IP.
- Cada petición lleva la hora y un número único: no se pueden repetir peticiones grabadas.
- La página del bridge (con los QR) **solo abre desde el mismo servidor** (`localhost`).
- `bridge/data/` guarda la sesión de WhatsApp y la clave: **trátalo como una contraseña**, no lo subas a GitHub (ya está en `.gitignore`).
- ¿Perdiste la Vita? En el servidor: `sudo rm data/pair.json && sudo docker compose restart` (o el `.bat` "Nuevo codigo de emparejamiento" en Windows) y vuelve a emparejar. La clave vieja deja de servir.

## Compilar la app de la Vita

Con VitaSDK (en WSL o Linux):

```bash
vdpm install libvita2d freetype libpng libjpeg-turbo zlib bzip2
cd vita
mkdir -p build && cd build
cmake ..
make
```

Sale `VitaWA.vpk` en `vita/build/`. También lo compila GitHub Actions (`.github/workflows/build-vpk.yml`).

## Bridge para desarrollo

Requiere Node 20+.

```bash
cd bridge
npm install
npm start
```

Abre `http://localhost:8787` en esa misma PC para ver los QR. Variables: `PORT` (8787), `PUBLIC_URL` (la dirección que va dentro del QR, por defecto la IP de la red local), `DATA_DIR`, `PAIR_RESET=1`.

## Estructura

```
bridge/   server.mjs (API), channel.mjs (canal cifrado), audio.mjs (Opus ⇄ μ-law)
          Dockerfile, docker-compose.yml, instalar-servidor.sh, .bat para Windows
vita/     src/main.c + scr_*.c (pantallas), ui.c (dibujo), sync.c (datos), chan.c (cifrado),
          audio.c, camera.c (+ lector QR), touch.c, gallery.c
```

## Créditos

- [Baileys](https://github.com/WhiskeySockets/Baileys) (MIT) — protocolo de WhatsApp Web.
- [Twemoji](https://github.com/jdecked/twemoji) — gráficos CC-BY 4.0.
- [M PLUS 1p](https://fonts.google.com/specimen/M+PLUS+1p) — SIL Open Font License.
- [Monocypher](https://monocypher.org) (BSD-2 / CC0) y [@noble/ciphers](https://github.com/paulmillr/noble-ciphers) (MIT) — cifrado.
- [quirc](https://github.com/dlbeer/quirc) (ISC) — lector de QR en la Vita.
- [vita2d](https://github.com/xerpi/libvita2d) y VitaSDK.
