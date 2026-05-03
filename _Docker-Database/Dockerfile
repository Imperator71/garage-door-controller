FROM node:18-alpine

WORKDIR /app
COPY package*.json ./
RUN npm install --omit=dev
COPY . .

# Data volume mount point -
RUN mkdir -p /data

EXPOSE 8080
CMD ["node", "server.js"]
