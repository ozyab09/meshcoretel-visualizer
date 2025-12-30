# Native MeshCoreTel Visualizer (Linux)

This client renders the MeshCoreTel map and overlays using SDL2. It connects to the local Node server for `/api/*` and `/sse`.

## Dependencies (Debian/Ubuntu)

```bash
sudo apt-get install -y \
  build-essential cmake pkg-config \
  libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev \
  libcurl4-openssl-dev
```

## Build

```bash
cmake -S native/linux -B native/linux/build
cmake --build native/linux/build
```

## Run

Start the server in one terminal:

```bash
npm install
npm run start:server
```

Then run the native client in another terminal:

```bash
./native/linux/build/meshcoretel-viewer
```

## Configuration

- `MESHCORETEL_SERVER_URL` (default: `http://localhost:3000`)
- `MESHCORETEL_FONT_PATH` (default: `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`)

## Controls

- `R` resets the view to Moscow.
- `A` toggles animations.
- Left click selects a node.

## Notes

- Propagation paths are rendered from `/sse` events using short node tokens mapped to known nodes.
- Logs are written to `native/linux/client.log` for troubleshooting.
