# Guía: dejar el bridge en la nube gratis (Oracle Cloud Always Free)

Así la Vita funciona estés donde estés, sin prender la PC ni hacer `npm install`. Se hace **una sola vez** (unos 30–40 minutos).

## 0. Lo que necesitas

- Una cuenta de **Oracle Cloud** (pide tarjeta para verificar identidad; el plan Always Free no cobra).
- Este proyecto (el zip o tu repo de GitHub).
- Tu teléfono con WhatsApp, y la Vita con VitaWA instalada.

## 1. Crear la cuenta

1. Entra a **cloud.oracle.com** → *Sign up*.
2. Elige como *Home Region* la más cercana (por ejemplo **US East (Ashburn)** o **Mexico Central (Querétaro)**). No se puede cambiar después.
3. Verifica con la tarjeta. Oracle hace un cargo temporal pequeño que luego devuelve.

## 2. Crear la máquina virtual

1. Menú ☰ → **Compute → Instances → Create instance**.
2. **Image**: *Canonical Ubuntu 24.04* (o 22.04).
3. **Shape**: *Ampere* → **VM.Standard.A1.Flex** con **1 OCPU y 6 GB** de RAM (sobra; es gratis hasta 4 OCPU / 24 GB).
   - Si dice *Out of capacity*, prueba otro *Availability Domain* o vuelve a intentar más tarde. Como alternativa, **VM.Standard.E2.1.Micro** (AMD) también es gratis y alcanza.
4. **Networking**: deja que cree una VCN nueva con subred pública y **Assign a public IPv4 address**.
5. **SSH keys**: *Generate a key pair for me* → **descarga la llave privada** (`.key`). Sin ella no entras.
6. **Create**. Cuando diga *Running*, copia la **Public IP address**.

## 3. Abrir el puerto 8787

1. En la instancia → **Primary VNIC → Subnet** → **Security Lists** → *Default Security List*.
2. **Add Ingress Rules**:
   - Source CIDR: `0.0.0.0/0`
   - IP Protocol: TCP
   - Destination Port Range: `8787`
3. Guardar. (El firewall interno de Ubuntu lo abre el script del paso 5.)

> Es seguro tenerlo abierto: sin la clave del QR nadie puede usar el bridge.

## 4. Entrar al servidor y subir el proyecto

Desde la terminal de tu PC (PowerShell en Windows):

```bash
ssh -i ruta/a/tu-llave.key ubuntu@IP_PUBLICA
```

(Si Windows se queja de permisos de la llave: clic derecho en el archivo → Propiedades → Seguridad → deja solo a tu usuario.)

Sube el proyecto de una de estas dos formas:

**a) Con GitHub** (si lo subiste a un repo, privado está bien):
```bash
git clone https://github.com/TU_USUARIO/vitawa.git
cd vitawa/bridge
```

**b) Con el zip**, desde tu PC (otra terminal):
```bash
scp -i ruta/a/tu-llave.key VitaWA-fase3.zip ubuntu@IP_PUBLICA:~
```
y en el servidor:
```bash
sudo apt-get update && sudo apt-get install -y unzip
unzip VitaWA-fase3.zip -d vitawa && cd vitawa/bridge
```

## 5. Instalar y arrancar

```bash
bash instalar-servidor.sh
```

El script instala Docker, detecta la IP pública, abre el puerto, construye y arranca el bridge. Al final imprime el **QR de emparejamiento** en la terminal (agranda la ventana si sale cortado).

El bridge queda corriendo solo y **se reinicia solo** si el servidor se reinicia.

## 6. Emparejar la Vita

1. Abre VitaWA en la Vita: sale la cámara.
2. Apunta al QR de la terminal (o con △ cambia de cámara).
3. Luego la Vita muestra el **QR de WhatsApp**: en el teléfono, *Ajustes → Dispositivos vinculados → Vincular un dispositivo* y escanéalo.

¿La cámara no lo lee? En la terminal, debajo del QR, sale un texto `VWA1 http://… clave`. En la Vita pulsa □ y escríbelo tal cual.

## Comandos útiles (en la carpeta `vitawa/bridge`)

| Para | Comando |
|---|---|
| Ver el QR otra vez / el log | `sudo docker compose logs --tail 80` |
| Reiniciar | `sudo docker compose restart` |
| Actualizar a una versión nueva | `git pull && sudo docker compose up -d --build` |
| Nuevo código (perdiste la Vita) | `sudo rm data/pair.json && sudo docker compose restart` |
| Desvincular WhatsApp por completo | `sudo docker compose down && sudo rm -rf data/auth && sudo docker compose up -d` |

## Notas

- **Máquinas "ociosas"**: Oracle puede reclamar instancias Always Free que pasan 7 días casi sin usar CPU. El bridge usa poco, así que puede pasar. Para evitarlo, pasa la cuenta a **Pay As You Go** (sigues sin pagar mientras uses solo recursos Always Free; esas cuentas no se reclaman). Pon una alerta de presupuesto de $1 por si acaso.
- **`data/`** guarda la sesión de WhatsApp y la clave del canal. Si haces backup, guárdalo como una contraseña.
- **Sin nube**: en Windows también puedes usar la carpeta *VitaWA-Bridge-Windows* (doble clic en `Iniciar VitaWA.bat`), pero solo funciona mientras la PC esté prendida y, fuera de tu casa, necesitarías abrir el puerto en el router.
