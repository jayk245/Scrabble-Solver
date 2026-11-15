import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

// The C++ server (apps/server) listens here by default. In development the
// browser talks only to the Vite dev server, which forwards /api requests,
// so the page and the API share an origin and no CORS setup is needed.
const API_TARGET = 'http://127.0.0.1:8080';

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      '/api': { target: API_TARGET, changeOrigin: true },
    },
  },
  build: {
    outDir: 'dist',
    sourcemap: true,
  },
});
