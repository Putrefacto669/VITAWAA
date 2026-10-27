#!/usr/bin/env bash
# Instala el bridge de VitaWA en un servidor Ubuntu (por ejemplo Oracle Cloud Always Free).
# Uso:  bash instalar-servidor.sh
set -euo pipefail
cd "$(dirname "$0")"

echo "== VitaWA: instalando el bridge =="

if ! command -v docker >/dev/null 2>&1; then
  echo "-> Instalando Docker (tarda un par de minutos)..."
  curl -fsSL https://get.docker.com | sudo sh
fi

IP="$(curl -fsS https://api.ipify.org || curl -fsS https://ifconfig.me || true)"
if [ -z "$IP" ]; then
  read -rp "No pude averiguar la IP publica. Escribela: " IP
fi
echo "PUBLIC_URL=http://$IP:8787" > .env
echo "-> La Vita se conectara a http://$IP:8787"

# Las imagenes de Ubuntu de Oracle bloquean todos los puertos menos SSH con iptables
if sudo iptables -S INPUT 2>/dev/null | grep -q -- "-j REJECT"; then
  if ! sudo iptables -C INPUT -p tcp --dport 8787 -j ACCEPT 2>/dev/null; then
    sudo iptables -I INPUT -p tcp --dport 8787 -j ACCEPT
    if command -v netfilter-persistent >/dev/null 2>&1; then sudo netfilter-persistent save; fi
  fi
  echo "-> Abri el puerto 8787 en el firewall del sistema"
fi

mkdir -p data
sudo docker compose up -d --build
echo "-> Esperando a que arranque..."
sleep 8
sudo docker compose logs --tail 80
echo
echo "Listo. Escanea el codigo de arriba con la camara de la Vita."
echo "Para verlo otra vez:   sudo docker compose logs --tail 80"
echo "Para actualizar:       git pull && sudo docker compose up -d --build"
