# Build from the repository root: the bot reads the docs in website/.
FROM node:24-slim
ENV COREPACK_ENABLE_DOWNLOAD_PROMPT=0
RUN corepack enable
WORKDIR /app/bot
COPY bot/package.json bot/pnpm-lock.yaml ./
RUN pnpm install --frozen-lockfile --prod
COPY bot/src ./src
COPY website/src/content/docs /app/website/src/content/docs
CMD ["node", "src/index.ts"]
