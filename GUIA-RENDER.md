# Bridge en Render (la forma más rápida)

Render solo atiende por **https://**. Desde la versión 3.5 la Vita habla https (con curl), así que funciona directo.

1. Sube el contenido de la carpeta `bridge/` a tu repo (en la raíz) **o** usa este repo completo con el `render.yaml` (ya trae `rootDir: bridge`).
2. En Render: *New → Web Service* (o *Blueprint* con `render.yaml`):
   - Runtime **Docker**, plan **Starter** (el Free se duerme y no tiene disco).
   - **Disk**: Mount Path `/data`, 1 GB.
   - Variables: `DATA_DIR=/data` y `ADMIN_TOKEN=` una clave larga (letras y números).
3. Cuando diga **Live**, abre `https://TU-APP.onrender.com/?k=TU_ADMIN_TOKEN`. Ahí sale el **QR de emparejamiento** de la Vita.
   - Render pone la dirección pública solo (`RENDER_EXTERNAL_URL`); no hace falta `PUBLIC_URL`.
   - Si no puedes escanearlo, el mismo texto `VWA1 https://… clave` sale en esa página y en los *Logs*: en la Vita pulsa □ y escríbelo.
4. En la Vita: VitaWA → apunta la cámara a ese QR → listo.
5. Vincula WhatsApp: en la Vita elige **con número** (código de 8 caracteres) o con QR.

La sesión de WhatsApp y la llave de emparejamiento viven en el disco `/data`: redeploys no la borran.
