# MeshCoreTel Network Visualizer

A real-time visualization tool for the MeshCoreTel network, displaying nodes, connections, and data propagation across the network.

## Features

- Real-time visualization of MeshCoreTel network nodes
- Animated propagation paths showing data transmission (native UI + web)
- Different colors for different node types:
  - Green: Chat Nodes
  - Blue: Repeaters
  - Yellow: Room Servers
  - Red: Sensors
- Live packet information display
- Interactive map centered on Moscow
- WebSocket integration for real-time updates

## Requirements

- Node.js (v14 or higher)
- npm or yarn
- Linux build tooling for native UI: CMake, SDL2, SDL2_image, SDL2_ttf, libcurl

## Installation

1. Clone the repository:
```bash
git clone <repository-url>
cd meshcoretel-visualizer
```

2. Install dependencies:
```bash
npm install
```

## Running the Application

1. Build the native client (Linux):
```bash
cmake -S native -B native/build
cmake --build native/build
```

2. Start the server + native UI:
```bash
npm start
```

If you prefer to run only the server (for web access), use:
```bash
npm run start:server
```

To run just the native client against an already running server:
```bash
./native/build/meshcoretel-viewer
```

## Project Structure

```
├── server.js           # Main server implementation
├── native/             # SDL2 client (Linux)
├── scripts/            # Helper scripts
├── public/
│   ├── index.html      # Main HTML page
│   └── ...
├── package.json        # Project dependencies and scripts
└── README.md           # This file
```

## Configuration

The application connects to the MeshCoreTel API endpoints automatically. No additional configuration is required for basic operation.

Optional environment variables:
- `MESHCORETEL_SERVER_URL` (default `http://localhost:3000`)
- `MESHCORETEL_FONT_PATH` (default `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`)

## API Endpoints

- `/api/adverts` - Retrieves all network nodes with pagination
- `/api/observers` - Gets observer information
- `/api/packets` - Fetches packet data
- `/api/propagations` - Gets propagation data
- `/sse` - Server-Sent Events for real-time updates

## How It Works

The application connects to the MeshCoreTel network and visualizes:
1. Network nodes on a map centered on Moscow
2. Real-time data transmission between nodes
3. Propagation paths when messages travel through the network
4. Status information and statistics

The native client subscribes to `/sse` and renders propagation links and packet events without a browser process.

## License

MIT
