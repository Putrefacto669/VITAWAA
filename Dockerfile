# Bridge de VitaWA para un servidor (Oracle Cloud, VPS, Raspberry Pi...)
FROM node:22-bookworm-slim
WORKDIR /app
ENV NODE_ENV=production DATA_DIR=/app/data
# En Render: monta el disco en /data y pon DATA_DIR=/data
COPY package.json package-lock.json ./
COPY scripts ./scripts
RUN npm ci --omit=dev && npm cache clean --force
COPY server.mjs channel.mjs audio.mjs ./
# Render pasa PORT (10000); en tu servidor se usa 8787
EXPOSE 8787
CMD ["node", "server.mjs"]
