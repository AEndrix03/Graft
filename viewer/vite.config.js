import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';

export default defineConfig({
  plugins: [vue()],
  build: {
    target: 'es2020',
    sourcemap: false,
    minify: 'esbuild',
    rollupOptions: {
      output: {
        // Stable asset directory matches what the daemon's static handler
        // accepts under "/assets/*".
        assetFileNames: 'assets/[name]-[hash][extname]',
        chunkFileNames: 'assets/[name]-[hash].js',
        entryFileNames: 'assets/[name]-[hash].js',
      },
    },
  },
  server: {
    proxy: {
      // During `npm run dev` the daemon is on :9977; proxy /v1/* there.
      '/v1': 'http://127.0.0.1:9977',
    },
  },
});
