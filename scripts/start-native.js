const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const repoRoot = path.resolve(__dirname, '..');
const binaryPath = path.join(repoRoot, 'native', 'build', 'meshcoretel-viewer');

if (!fs.existsSync(binaryPath)) {
  console.error('Native client not built. Build it first:');
  console.error('  cmake -S native -B native/build');
  console.error('  cmake --build native/build');
  process.exit(1);
}

const http = require('http');

const server = spawn(process.execPath, [path.join(repoRoot, 'server.js')], {
  stdio: 'inherit',
});

const waitForServer = (attempt = 0) =>
  new Promise((resolve) => {
    const req = http.get('http://localhost:3000/api/adverts', (res) => {
      res.resume();
      if (res.statusCode && res.statusCode >= 200 && res.statusCode < 500) {
        resolve(true);
      } else {
        resolve(false);
      }
    });
    req.on('error', () => resolve(false));
    req.setTimeout(1000, () => {
      req.destroy();
      resolve(false);
    });
  }).then((ok) => {
    if (ok) {
      return;
    }
    const delayMs = Math.min(1000 + attempt * 500, 5000);
    return new Promise((r) => setTimeout(r, delayMs)).then(() =>
      waitForServer(attempt + 1)
    );
  });

let client = null;
let shuttingDown = false;

const shutdown = (code) => {
  shuttingDown = true;
  if (!server.killed) {
    server.kill('SIGINT');
  }
  if (client && !client.killed) {
    client.kill('SIGINT');
  }
  if (typeof code === 'number') {
    process.exit(code);
  }
};

waitForServer()
  .then(() => {
    const startClient = () => {
      if (shuttingDown) {
        return;
      }
      client = spawn(binaryPath, { stdio: 'inherit' });
      client.on('error', (err) => {
        console.error(`Native client failed to start: ${err.message}`);
      });
      client.on('exit', (code) => {
        if (shuttingDown) {
          return;
        }
        console.error(`Native client exited (${code || 0}), restarting in 2s...`);
        setTimeout(startClient, 2000);
      });
    };
    startClient();
  })
  .catch(() => shutdown(1));

server.on('exit', (code) => shutdown(code || 1));

process.on('SIGINT', () => shutdown(0));
process.on('SIGTERM', () => shutdown(0));
